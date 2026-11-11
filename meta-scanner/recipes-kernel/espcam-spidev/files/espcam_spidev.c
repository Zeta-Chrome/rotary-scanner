// SPDX-License-Identifier: GPL-2.0
/*
 * espcam_spidev - V4L2 capture driver for an ESP32-CAM acting as SPI slave
 * The frame is 320x240 @ 16 bpp (RGB565 by default, YUYV via "yuyv=1").
 * Frames are exposed through /dev/videoN
 * The LED is exposed as V4L2_CID_FLASH_LED_MODE (off / torch).
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/version.h>

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#define DRV_NAME "espcam_spidev"

#define ESPCAM_CAPTURE_CMD 0x55
#define ESPCAM_FLASH_ON_CMD 0xAA
#define ESPCAM_FLASH_OFF_CMD 0x33

#define ESPCAM_WIDTH 320
#define ESPCAM_HEIGHT 240
#define ESPCAM_BPP 2
#define ESPCAM_FRAME_SIZE                                                      \
	(ESPCAM_WIDTH * ESPCAM_HEIGHT * ESPCAM_BPP) /* 153600 */
#define ESPCAM_MAX_SPEED_HZ 10000000

static bool yuyv;
module_param(yuyv, bool, 0444);
MODULE_PARM_DESC(yuyv,
				 "ESP32 sends YUV422 (YUYV) instead of RGB565 (default 0)");

static unsigned int timeout_ms = 1000;
module_param(timeout_ms, uint, 0644);
MODULE_PARM_DESC(timeout_ms,
				 "Max wait for ESP32 'frame ready' IRQ after capture cmd");

static const u32 espcam_formats[] = { V4L2_PIX_FMT_RGB565, V4L2_PIX_FMT_YUYV };

struct espcam_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct espcam {
	struct spi_device *spi;
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue vq;
	struct v4l2_ctrl_handler hdl;

	struct mutex lock;	   /* V4L2 / vb2 serialisation */
	struct mutex spi_lock; /* keeps cmd+IRQ+readout atomic vs. flash cmds */

	spinlock_t qlock; /* protects bufs */
	struct list_head bufs;
	wait_queue_head_t wq;

	struct completion frame_rdy;
	struct task_struct *kthread;
	bool stopping;
	u32 sequence;
	u32 pixfmt;

	u8 *txbuf; /* DMA-safe single byte */
	u8 *rxbuf; /* DMA-safe frame bounce buffer */
};

static int espcam_send_cmd_locked(struct espcam *cam, u8 cmd)
{
	cam->txbuf[0] = cmd;
	return spi_write(cam->spi, cam->txbuf, 1);
}

static int espcam_send_cmd(struct espcam *cam, u8 cmd)
{
	int ret;

	mutex_lock(&cam->spi_lock);
	ret = espcam_send_cmd_locked(cam, cmd);
	mutex_unlock(&cam->spi_lock);
	return ret;
}

static irqreturn_t espcam_irq(int irq, void *dev_id)
{
	struct espcam *cam = dev_id;

	complete(&cam->frame_rdy);
	return IRQ_HANDLED;
}

/* Capture one frame into cam->rxbuf */
static int espcam_grab(struct espcam *cam)
{
	struct spi_transfer xfer = {
		.rx_buf = cam->rxbuf,
		.len = ESPCAM_FRAME_SIZE,
		.speed_hz = cam->spi->max_speed_hz,
		.bits_per_word = 8,
	};
	int ret;

	mutex_lock(&cam->spi_lock);

	// Drop stale IRQs; stop path sets 'stopping' *then* completes.
	reinit_completion(&cam->frame_rdy);
	if (READ_ONCE(cam->stopping)) {
		ret = -ECANCELED;
		goto out;
	}

	ret = espcam_send_cmd_locked(cam, ESPCAM_CAPTURE_CMD);
	if (ret) {
		dev_err(&cam->spi->dev, "capture cmd failed: %d\n", ret);
		goto out;
	}

	if (!wait_for_completion_timeout(&cam->frame_rdy,
									 msecs_to_jiffies(timeout_ms))) {
		dev_warn(&cam->spi->dev, "timeout waiting for frame-ready IRQ\n");
		ret = -ETIMEDOUT;
		goto out;
	}
	if (READ_ONCE(cam->stopping)) {
		ret = -ECANCELED;
		goto out;
	}

	ret = spi_sync_transfer(cam->spi, &xfer, 1); /* DMA read, 153600 B */
	if (ret)
		dev_err(&cam->spi->dev, "frame transfer failed: %d\n", ret);
out:
	mutex_unlock(&cam->spi_lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Capture thread                                                      */
/* ------------------------------------------------------------------ */

static struct espcam_buf *espcam_pop_buf(struct espcam *cam)
{
	struct espcam_buf *buf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&cam->qlock, flags);
	if (!list_empty(&cam->bufs)) {
		buf = list_first_entry(&cam->bufs, struct espcam_buf, list);
		list_del(&buf->list);
	}
	spin_unlock_irqrestore(&cam->qlock, flags);
	return buf;
}

static bool espcam_has_work(struct espcam *cam)
{
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(&cam->qlock, flags);
	ret = !list_empty(&cam->bufs);
	spin_unlock_irqrestore(&cam->qlock, flags);
	return ret || READ_ONCE(cam->stopping);
}

static int espcam_thread(void *data)
{
	struct espcam *cam = data;

	while (!READ_ONCE(cam->stopping)) {
		struct espcam_buf *buf;
		int ret;

		if (wait_event_interruptible(cam->wq, espcam_has_work(cam)))
			continue;
		if (READ_ONCE(cam->stopping))
			break;

		buf = espcam_pop_buf(cam);
		if (!buf)
			continue;

		ret = espcam_grab(cam);
		if (ret) {
			vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
			if (ret != -ECANCELED)
				msleep(20); /* don't spin on a dead slave */
			continue;
		}

		memcpy(vb2_plane_vaddr(&buf->vb.vb2_buf, 0), cam->rxbuf,
			   ESPCAM_FRAME_SIZE);
		vb2_set_plane_payload(&buf->vb.vb2_buf, 0, ESPCAM_FRAME_SIZE);
		buf->vb.vb2_buf.timestamp = ktime_get_ns();
		buf->vb.sequence = cam->sequence++;
		buf->vb.field = V4L2_FIELD_NONE;
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* videobuf2                                                           */
/* ------------------------------------------------------------------ */

static int espcam_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
							  unsigned int *nplanes, unsigned int sizes[],
							  struct device *alloc_devs[])
{
	if (*nplanes)
		return sizes[0] < ESPCAM_FRAME_SIZE ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = ESPCAM_FRAME_SIZE;
	return 0;
}

static int espcam_buf_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < ESPCAM_FRAME_SIZE)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, ESPCAM_FRAME_SIZE);
	return 0;
}

static void espcam_buf_queue(struct vb2_buffer *vb)
{
	struct espcam *cam = vb2_get_drv_priv(vb->vb2_queue);
	struct espcam_buf *buf =
		container_of(to_vb2_v4l2_buffer(vb), struct espcam_buf, vb);
	unsigned long flags;

	spin_lock_irqsave(&cam->qlock, flags);
	list_add_tail(&buf->list, &cam->bufs);
	spin_unlock_irqrestore(&cam->qlock, flags);
	wake_up(&cam->wq);
}

static void espcam_return_bufs(struct espcam *cam, enum vb2_buffer_state state)
{
	struct espcam_buf *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&cam->qlock, flags);
	list_for_each_entry_safe(buf, tmp, &cam->bufs, list)
	{
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&cam->qlock, flags);
}

static int espcam_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct espcam *cam = vb2_get_drv_priv(vq);

	cam->sequence = 0;
	WRITE_ONCE(cam->stopping, false);
	cam->kthread = kthread_run(espcam_thread, cam, "espcam_grab");
	if (IS_ERR(cam->kthread)) {
		int ret = PTR_ERR(cam->kthread);

		cam->kthread = NULL;
		espcam_return_bufs(cam, VB2_BUF_STATE_QUEUED);
		return ret;
	}
	return 0;
}

static void espcam_stop_streaming(struct vb2_queue *vq)
{
	struct espcam *cam = vb2_get_drv_priv(vq);

	if (cam->kthread) {
		WRITE_ONCE(cam->stopping, true);
		complete(&cam->frame_rdy); /* break out of IRQ wait */
		wake_up(&cam->wq);
		kthread_stop(cam->kthread);
		cam->kthread = NULL;
	}
	espcam_return_bufs(cam, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops espcam_vb2_ops = {
	.queue_setup = espcam_queue_setup,
	.buf_prepare = espcam_buf_prepare,
	.buf_queue = espcam_buf_queue,
	.start_streaming = espcam_start_streaming,
	.stop_streaming = espcam_stop_streaming,
};

static int espcam_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct espcam *cam = container_of(ctrl->handler, struct espcam, hdl);

	switch (ctrl->id) {
	case V4L2_CID_FLASH_LED_MODE:
		return espcam_send_cmd(cam, ctrl->val == V4L2_FLASH_LED_MODE_TORCH ?
										ESPCAM_FLASH_ON_CMD :
										ESPCAM_FLASH_OFF_CMD);
	}
	return -EINVAL;
}

static const struct v4l2_ctrl_ops espcam_ctrl_ops = {
	.s_ctrl = espcam_s_ctrl,
};

static bool espcam_fmt_ok(u32 fourcc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(espcam_formats); i++)
		if (espcam_formats[i] == fourcc)
			return true;
	return false;
}

static void espcam_fill_fmt(struct espcam *cam, struct v4l2_pix_format *pix,
							u32 fourcc)
{
	if (!espcam_fmt_ok(fourcc))
		fourcc = cam->pixfmt;

	pix->width = ESPCAM_WIDTH;
	pix->height = ESPCAM_HEIGHT;
	pix->pixelformat = fourcc;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = ESPCAM_WIDTH * ESPCAM_BPP;
	pix->sizeimage = ESPCAM_FRAME_SIZE;
	pix->colorspace = V4L2_COLORSPACE_SRGB;
}

static int espcam_querycap(struct file *file, void *priv,
						   struct v4l2_capability *cap)
{
	struct espcam *cam = video_drvdata(file);

	strscpy(cap->driver, DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "ESP32-CAM SPI", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "spi:%s",
			 dev_name(&cam->spi->dev));
	return 0;
}

static int espcam_enum_fmt(struct file *file, void *priv,
						   struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(espcam_formats))
		return -EINVAL;
	f->pixelformat = espcam_formats[f->index];
	return 0;
}

static int espcam_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct espcam *cam = video_drvdata(file);

	espcam_fill_fmt(cam, &f->fmt.pix, cam->pixfmt);
	return 0;
}

static int espcam_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct espcam *cam = video_drvdata(file);

	espcam_fill_fmt(cam, &f->fmt.pix, f->fmt.pix.pixelformat);
	return 0;
}

static int espcam_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct espcam *cam = video_drvdata(file);

	if (vb2_is_busy(&cam->vq))
		return -EBUSY;
	espcam_fill_fmt(cam, &f->fmt.pix, f->fmt.pix.pixelformat);
	cam->pixfmt = f->fmt.pix.pixelformat;
	return 0;
}

static int espcam_enum_framesizes(struct file *file, void *priv,
								  struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index || !espcam_fmt_ok(fsize->pixel_format))
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = ESPCAM_WIDTH;
	fsize->discrete.height = ESPCAM_HEIGHT;
	return 0;
}

static int espcam_enum_input(struct file *file, void *priv,
							 struct v4l2_input *inp)
{
	if (inp->index)
		return -EINVAL;
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	strscpy(inp->name, "ESP32-CAM", sizeof(inp->name));
	return 0;
}

static int espcam_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int espcam_s_input(struct file *file, void *priv, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static const struct v4l2_ioctl_ops espcam_ioctl_ops = {
	.vidioc_querycap = espcam_querycap,
	.vidioc_enum_fmt_vid_cap = espcam_enum_fmt,
	.vidioc_g_fmt_vid_cap = espcam_g_fmt,
	.vidioc_try_fmt_vid_cap = espcam_try_fmt,
	.vidioc_s_fmt_vid_cap = espcam_s_fmt,
	.vidioc_enum_framesizes = espcam_enum_framesizes,
	.vidioc_enum_input = espcam_enum_input,
	.vidioc_g_input = espcam_g_input,
	.vidioc_s_input = espcam_s_input,

	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,

	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations espcam_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/* ------------------------------------------------------------------ */
/* SPI driver                                                          */
/* ------------------------------------------------------------------ */

static int espcam_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct espcam *cam;
	struct vb2_queue *vq;
	struct video_device *vdev;
	int ret;

	BUILD_BUG_ON(ESPCAM_FRAME_SIZE != 153600);

	cam = devm_kzalloc(dev, sizeof(*cam), GFP_KERNEL);
	if (!cam)
		return -ENOMEM;

	cam->txbuf = devm_kzalloc(dev, 16, GFP_KERNEL);
	cam->rxbuf = devm_kmalloc(dev, ESPCAM_FRAME_SIZE, GFP_KERNEL);
	if (!cam->txbuf || !cam->rxbuf)
		return -ENOMEM;

	cam->spi = spi;
	cam->pixfmt = yuyv ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_RGB565;
	mutex_init(&cam->lock);
	mutex_init(&cam->spi_lock);
	spin_lock_init(&cam->qlock);
	INIT_LIST_HEAD(&cam->bufs);
	init_waitqueue_head(&cam->wq);
	init_completion(&cam->frame_rdy);
	spi_set_drvdata(spi, cam);

	/* SPI setup: mode 0, 8 bit, <= 10 MHz */
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	if (!spi->max_speed_hz || spi->max_speed_hz > ESPCAM_MAX_SPEED_HZ)
		spi->max_speed_hz = ESPCAM_MAX_SPEED_HZ;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "spi_setup failed\n");

	if (spi->irq <= 0)
		return dev_err_probe(dev, -EINVAL,
							 "missing 'interrupts' (frame-ready GPIO)\n");

	ret = devm_request_irq(dev, spi->irq, espcam_irq, IRQF_TRIGGER_RISING,
						   DRV_NAME, cam);
	if (ret)
		return dev_err_probe(dev, ret, "request_irq failed\n");

	/* V4L2 */
	strscpy(cam->v4l2_dev.name, DRV_NAME, sizeof(cam->v4l2_dev.name));
	ret = v4l2_device_register(dev, &cam->v4l2_dev);
	if (ret)
		return ret;

	v4l2_ctrl_handler_init(&cam->hdl, 1);
	v4l2_ctrl_new_std_menu(&cam->hdl, &espcam_ctrl_ops, V4L2_CID_FLASH_LED_MODE,
						   V4L2_FLASH_LED_MODE_TORCH,
						   BIT(V4L2_FLASH_LED_MODE_FLASH),
						   V4L2_FLASH_LED_MODE_NONE);
	if (cam->hdl.error) {
		ret = cam->hdl.error;
		goto err_ctrl;
	}
	cam->v4l2_dev.ctrl_handler = &cam->hdl;

	vq = &cam->vq;
	vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_READ;
	vq->drv_priv = cam;
	vq->buf_struct_size = sizeof(struct espcam_buf);
	vq->ops = &espcam_vb2_ops;
	vq->mem_ops = &vb2_vmalloc_memops;
	vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vq->min_buffers_needed = 1;
	vq->lock = &cam->lock;
	vq->dev = dev;
	ret = vb2_queue_init(vq);
	if (ret)
		goto err_ctrl;

	vdev = &cam->vdev;
	strscpy(vdev->name, DRV_NAME, sizeof(vdev->name));
	vdev->fops = &espcam_fops;
	vdev->ioctl_ops = &espcam_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->v4l2_dev = &cam->v4l2_dev;
	vdev->queue = vq;
	vdev->lock = &cam->lock;
	vdev->device_caps =
		V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
	video_set_drvdata(vdev, cam);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(dev, "video_register_device failed: %d\n", ret);
		goto err_ctrl;
	}

	/* Known LED state at start (best effort; slave may not be up yet) */
	espcam_send_cmd(cam, ESPCAM_FLASH_OFF_CMD);

	dev_info(dev,
			 "ESP32-CAM registered as %s (%ux%u, %zu bytes/frame, %u Hz)\n",
			 video_device_node_name(vdev), ESPCAM_WIDTH, ESPCAM_HEIGHT,
			 (size_t)ESPCAM_FRAME_SIZE, spi->max_speed_hz);
	return 0;

err_ctrl:
	v4l2_ctrl_handler_free(&cam->hdl);
	v4l2_device_unregister(&cam->v4l2_dev);
	return ret;
}

static void espcam_remove(struct spi_device *spi)
{
	struct espcam *cam = spi_get_drvdata(spi);

	video_unregister_device(&cam->vdev);
	v4l2_ctrl_handler_free(&cam->hdl);
	v4l2_device_unregister(&cam->v4l2_dev);
}

static const struct of_device_id espcam_of_match[] = {
	{ .compatible = "abel,esp32cam" }, {}
};
MODULE_DEVICE_TABLE(of, espcam_of_match);

static const struct spi_device_id espcam_spi_ids[] = { { "esp32cam", 0 }, {} };
MODULE_DEVICE_TABLE(spi, espcam_spi_ids);

static struct spi_driver espcam_driver = {
	.driver = {
		.name		= DRV_NAME,
		.of_match_table	= espcam_of_match,
	},
	.id_table = espcam_spi_ids,
	.probe	  = espcam_probe,
	.remove	  = espcam_remove,
};
module_spi_driver(espcam_driver);

MODULE_DESCRIPTION("ESP32-CAM SPI-slave V4L2 capture driver");
MODULE_AUTHOR("abel");
MODULE_LICENSE("GPL");

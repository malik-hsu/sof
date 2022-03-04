// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2020 Intel Corporation. All rights reserved.
//
// Author: Marcin Rajwa <marcin.rajwa@linux.intel.com>

/*
 * A codec adapter component.
 */

/**
 * \file
 * \brief Processing component aimed to work with external codec libraries
 * \author Marcin Rajwa <marcin.rajwa@linux.intel.com>
 */

#include <sof/audio/buffer.h>
#include <sof/audio/component.h>
#include <sof/audio/ipc-config.h>
#include <sof/audio/codec_adapter/codec_adapter.h>
#include <sof/audio/pipeline.h>
#include <sof/common.h>
#include <sof/platform.h>
#include <sof/ut.h>

/**
 * \brief Create a codec adapter component.
 * \param[in] drv - component driver pointer.
 * \param[in] config - component ipc descriptor pointer.
 *
 * \return: a pointer to newly created codec adapter component.
 */
struct comp_dev *codec_adapter_new(const struct comp_driver *drv,
				   struct comp_ipc_config *config,
				   struct module_interface *interface,
				   void *spec)
{
	int ret;
	struct comp_dev *dev;
	struct processing_module *mod;
	struct ipc_config_process *ipc_codec_adapter = spec;

	comp_cl_dbg(drv, "codec_adapter_new() start");

	if (!config) {
		comp_cl_err(drv, "codec_adapter_new(), wrong input params! drv = %x config = %x",
			    (uint32_t)drv, (uint32_t)config);
		return NULL;
	}

	dev = comp_alloc(drv, sizeof(*dev));
	if (!dev) {
		comp_cl_err(drv, "codec_adapter_new(), failed to allocate memory for comp_dev");
		return NULL;
	}
	dev->ipc_config = *config;
	dev->drv = drv;

	mod = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM, sizeof(*mod));
	if (!mod) {
		comp_err(dev, "codec_adapter_new(), failed to allocate memory for module");
		rfree(dev);
		return NULL;
	}

	mod->dev = dev;

	comp_set_drvdata(dev, mod);

	/* Copy initial config */
	if (ipc_codec_adapter->size) {
		ret = module_load_config(dev, ipc_codec_adapter->data, ipc_codec_adapter->size);
		if (ret) {
			comp_err(dev, "codec_adapter_new() error %d: config loading has failed.",
				 ret);
			goto err;
		}
	}

	/* Init processing codec */
	ret = module_init(mod, interface);
	if (ret) {
		comp_err(dev, "codec_adapter_new() %d: codec initialization failed",
			 ret);
		goto err;
	}

	dev->state = COMP_STATE_READY;

	comp_dbg(dev, "codec_adapter_new() done");
	return dev;
err:
	rfree(mod);
	rfree(dev);
	return NULL;
}

/*
 * \brief Prepare a codec adapter component.
 * \param[in] dev - component device pointer.
 *
 * \return integer representing either:
 *	0 - success
 *	value < 0 - failure.
 */
int codec_adapter_prepare(struct comp_dev *dev)
{
	int ret;
	struct processing_module *mod = comp_get_drvdata(dev);
	struct module_data *md = &mod->priv;
	struct list_item *blist;
	uint32_t buff_periods;
	uint32_t buff_size; /* size of local buffer */

	comp_dbg(dev, "codec_adapter_prepare() start");

	/* Are we already prepared? */
	ret = comp_set_state(dev, COMP_TRIGGER_PREPARE);
	if (ret < 0)
		return ret;

	if (ret == COMP_STATUS_STATE_ALREADY_SET) {
		comp_warn(dev, "codec_adapter_prepare(): codec_adapter has already been prepared");
		return PPL_STATUS_PATH_STOP;
	}

	/* Prepare codec */
	ret = module_prepare(mod);
	if (ret) {
		comp_err(dev, "codec_adapter_prepare() error %x: codec prepare failed",
			 ret);

		return -EIO;
	}

	mod->deep_buff_bytes = 0;

	/* Codec is prepared, now we need to configure processing settings.
	 * If codec internal buffer is not equal to natural multiple of pipeline
	 * buffer we have a situation where CA have to deep buffer certain amount
	 * of samples on its start (typically few periods) in order to regularly
	 * generate output once started (same situation happens for compress streams
	 * as well).
	 */
	if (md->mpd.in_buff_size > mod->period_bytes) {
		buff_periods = (md->mpd.in_buff_size % mod->period_bytes) ?
			       (md->mpd.in_buff_size / mod->period_bytes) + 2 :
			       (md->mpd.in_buff_size / mod->period_bytes) + 1;
	} else {
		buff_periods = (mod->period_bytes % md->mpd.in_buff_size) ?
			       (mod->period_bytes / md->mpd.in_buff_size) + 2 :
			       (mod->period_bytes / md->mpd.in_buff_size) + 1;
	}

	/*
	 * deep_buffer_bytes is a measure of how many bytes we need to send to the DAI before
	 * the module starts producing samples. In a normal copy() walk it might be possible that
	 * the first period_bytes copied to input_buffer might not be enough for the processing
	 * to begin. So, in order to prevent the DAI from starving, it needs to be fed zeroes until
	 * the module starts processing and generating output samples.
	 */
	if (md->mpd.in_buff_size != mod->period_bytes)
		mod->deep_buff_bytes = MIN(mod->period_bytes, md->mpd.in_buff_size) * buff_periods;

	if (md->mpd.out_buff_size > mod->period_bytes) {
		buff_periods = (md->mpd.out_buff_size % mod->period_bytes) ?
			       (md->mpd.out_buff_size / mod->period_bytes) + 2 :
			       (md->mpd.out_buff_size / mod->period_bytes) + 1;
	} else {
		buff_periods = (mod->period_bytes % md->mpd.out_buff_size) ?
			       (mod->period_bytes / md->mpd.out_buff_size) + 2 :
			       (mod->period_bytes / md->mpd.out_buff_size) + 1;
	}

	/*
	 * It is possible that the module process() will produce more data than period_bytes but
	 * the DAI can consume only period_bytes every period. So, the local buffer needs to be
	 * large enough to save the produced output samples.
	 */
	buff_size = MAX(mod->period_bytes, md->mpd.out_buff_size) * buff_periods;

	/* compute number of input buffers */
	list_for_item(blist, &dev->bsource_list)
		mod->num_input_buffers++;

	/* compute number of output buffers */
	list_for_item(blist, &dev->bsink_list)
		mod->num_output_buffers++;

	if (!mod->num_input_buffers || !mod->num_output_buffers) {
		comp_err(dev, "codec_adapter_prepare(): invalid number of source/sink buffers");
		return -EINVAL;
	}

	/* Allocate local buffer */
	if (mod->local_buff) {
		ret = buffer_set_size(mod->local_buff, buff_size);
		if (ret < 0) {
			comp_err(dev, "codec_adapter_prepare(): buffer_set_size() failed, buff_size = %u",
				 buff_size);
			return ret;
		}
	} else {
		mod->local_buff = buffer_alloc(buff_size, SOF_MEM_CAPS_RAM, PLATFORM_DCACHE_ALIGN);
		if (!mod->local_buff) {
			comp_err(dev, "codec_adapter_prepare(): failed to allocate local buffer");
			return -ENOMEM;
		}

	}
	buffer_set_params(mod->local_buff, &mod->stream_params,
			  BUFFER_UPDATE_FORCE);
	buffer_reset_pos(mod->local_buff, NULL);

	comp_dbg(dev, "codec_adapter_prepare() done");

	return 0;
}

int codec_adapter_params(struct comp_dev *dev,
			 struct sof_ipc_stream_params *params)
{
	int ret;
	struct processing_module *mod = comp_get_drvdata(dev);

	ret = comp_verify_params(dev, 0, params);
	if (ret < 0) {
		comp_err(dev, "codec_adapter_params(): comp_verify_params() failed.");
		return ret;
	}

	ret = memcpy_s(&mod->stream_params, sizeof(struct sof_ipc_stream_params),
		       params, sizeof(struct sof_ipc_stream_params));
	assert(!ret);

	mod->period_bytes = params->sample_container_bytes *
			   params->channels * params->rate / 1000;
	return 0;
}

/**
 * Function to copy from source buffer to the module buffer
 * @source: source audio buffer stream
 * @buff: pointer to the module input buffer
 * @buff_size: size of the module input buffer
 * @bytes: number of bytes available in the source buffer
 */
static void
ca_copy_from_source_to_module(const struct audio_stream *source, void *buff, uint32_t buff_size,
			      size_t bytes)
{
	/* head_size - available data until end of source buffer */
	const int without_wrap = audio_stream_bytes_without_wrap(source, source->r_ptr);
	uint32_t head_size = MIN(bytes, without_wrap);
	/* tail_size - residual data to be copied starting from the beginning of the buffer */
	uint32_t tail_size = bytes - head_size;

	/* copy head_size to module buffer */
	memcpy_s(buff, buff_size, source->r_ptr, head_size);

	/* copy residual samples after wrap */
	if (tail_size)
		memcpy_s((char *)buff + head_size, buff_size,
			 audio_stream_wrap(source,
					   (char *)source->r_ptr + head_size),
					   tail_size);
}

/**
 * Function to copy processed samples from the module buffer to sink buffer
 * @sink: sink audio buffer stream
 * @buff: pointer to the module output buffer
 * @bytes: number of bytes available in the module output buffer
 */
static void
ca_copy_from_module_to_sink(const struct audio_stream *sink, void *buff, size_t bytes)
{
	/* head_size - free space until end of sink buffer */
	const int without_wrap = audio_stream_bytes_without_wrap(sink, sink->w_ptr);
	uint32_t head_size = MIN(bytes, without_wrap);
	/* tail_size - rest of the bytes that needs to be written
	 * starting from the beginning of the buffer
	 */
	uint32_t tail_size = bytes - head_size;

	/* copy "head_size" samples to sink buffer */
	memcpy_s(sink->w_ptr, sink->size, buff, head_size);

	/* copy rest of the samples after buffer wrap */
	if (tail_size)
		memcpy_s(audio_stream_wrap(sink, (char *)sink->w_ptr + head_size),
			 sink->size, (char *)buff + head_size, tail_size);
}

/**
 * \brief Generate zero samples of "bytes" size for the sink.
 * \param[in] sink - a pointer to sink buffer.
 * \param[in] bytes - number of zero bytes to produce.
 *
 * \return: none.
 */
static void generate_zeroes(struct comp_buffer *sink, uint32_t bytes)
{
	uint32_t tmp, copy_bytes = bytes;
	void *ptr;

	while (copy_bytes) {
		ptr = audio_stream_wrap(&sink->stream, sink->stream.w_ptr);
		tmp = audio_stream_bytes_without_wrap(&sink->stream,
						      ptr);
		tmp = MIN(tmp, copy_bytes);
		ptr = (char *)ptr + tmp;
		copy_bytes -= tmp;
	}
	comp_update_buffer_produce(sink, bytes);
}

int codec_adapter_copy(struct comp_dev *dev)
{
	int ret = 0;
	uint32_t bytes_to_process, copy_bytes, processed = 0, produced = 0;
	struct comp_buffer *source = list_first_item(&dev->bsource_list, struct comp_buffer,
						     sink_list);
	struct comp_buffer *sink = list_first_item(&dev->bsink_list, struct comp_buffer,
						    source_list);
	struct processing_module *mod = comp_get_drvdata(dev);
	struct module_data *md = &mod->priv;
	uint32_t codec_buff_size = md->mpd.in_buff_size;
	struct comp_buffer *local_buff = mod->local_buff;
	struct comp_copy_limits cl;
	struct input_stream_buffer *input_buffers;
	struct output_stream_buffer *output_buffers;
	struct list_item *blist;
	int i = 0;

	if (!source || !sink) {
		comp_err(dev, "codec_adapter_copy(): source/sink buffer not found");
		return -EINVAL;
	}

	comp_get_copy_limits_with_lock(source, local_buff, &cl);
	bytes_to_process = cl.frames * cl.source_frame_bytes;

	comp_dbg(dev, "codec_adapter_copy() start: codec_buff_size: %d, local_buff free: %d source avail %d",
		 codec_buff_size, local_buff->stream.free, source->stream.avail);

	/* allocate memory for input buffers */
	input_buffers = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM,
				sizeof(*input_buffers) * mod->num_input_buffers);
	if (!input_buffers) {
		comp_err(dev, "codec_adapter_copy(): failed to allocate input buffers");
		return -ENOMEM;
	}

	/* allocate memory for input buffer data and copy source samples */
	list_for_item(blist, &dev->bsource_list) {
		int frames, source_frame_bytes;

		source = container_of(blist, struct comp_buffer, sink_list);

		source = buffer_acquire(source);
		frames = audio_stream_avail_frames(&source->stream, &sink->stream);
		source_frame_bytes = audio_stream_frame_bytes(&source->stream);
		source = buffer_release(source);

		bytes_to_process = frames * source_frame_bytes;

		buffer_stream_invalidate(source, bytes_to_process);
		input_buffers[i].data = rballoc(0, SOF_MEM_CAPS_RAM, bytes_to_process);
		if (!input_buffers[i].data) {
			comp_err(mod->dev, "codec_adapter_copy(): Failed to alloc input buffer data");
			ret = -ENOMEM;
			goto free;
		}

		input_buffers[i].size = bytes_to_process;
		ca_copy_from_source_to_module(&source->stream, input_buffers[i].data,
					      input_buffers[i].size, bytes_to_process);
		i++;
	}

	/*
	 * allocate memory for output buffers. The memory for output buffer data should be
	 * allocated by the module depending on the number of output samples produced
	 */
	output_buffers = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM,
				 sizeof(*output_buffers) * mod->num_output_buffers);
	if (!output_buffers) {
		comp_err(dev, "codec_adapter_copy(): failed to allocate output buffers");
		goto free;
	}

	/*
	 * This should be removed once all codec implementations start using the passed
	 * input/output buffers
	 */
	source = list_first_item(&dev->bsource_list, struct comp_buffer, sink_list);
	bytes_to_process = cl.frames * cl.source_frame_bytes;

	/* Proceed only if we have enough data to fill the lib buffer
	 * completely. If you don't fill whole buffer
	 * the lib won't process it.
	 */
	if (bytes_to_process < codec_buff_size) {
		comp_dbg(dev, "codec_adapter_copy(): source has less data than codec buffer size - processing terminated.");
		goto db_verify;
	}

	if (!md->mpd.init_done) {
		buffer_stream_invalidate(source, codec_buff_size);
		ca_copy_from_source_to_module(&source->stream, md->mpd.in_buff,
					      md->mpd.in_buff_size, codec_buff_size);
		md->mpd.avail = codec_buff_size;
		ret = module_process(mod, input_buffers, mod->num_input_buffers,
				     output_buffers, mod->num_output_buffers);
		if (ret)
			goto out_free;

		bytes_to_process -= md->mpd.consumed;
		processed += md->mpd.consumed;
		comp_update_buffer_consume(source, md->mpd.consumed);
		if (bytes_to_process < codec_buff_size)
			goto db_verify;
	}

	buffer_stream_invalidate(source, codec_buff_size);
	ca_copy_from_source_to_module(&source->stream, md->mpd.in_buff,
				      md->mpd.in_buff_size, codec_buff_size);
	md->mpd.avail = codec_buff_size;
	ret = module_process(mod, input_buffers, mod->num_input_buffers,
			     output_buffers, mod->num_output_buffers);
	if (ret) {
		if (ret == -ENOSPC) {
			ret = 0;
			goto db_verify;
		}

		comp_err(dev, "codec_adapter_copy() error %x: lib processing failed",
			 ret);
		goto db_verify;
	} else if (md->mpd.produced == 0) {
		/* skipping as lib has not produced anything */
		comp_err(dev, "codec_adapter_copy() error %x: lib hasn't processed anything",
			 ret);
		goto db_verify;
	}
	ca_copy_from_module_to_sink(&local_buff->stream, md->mpd.out_buff, md->mpd.produced);

	bytes_to_process -= md->mpd.consumed;
	processed += md->mpd.consumed;
	produced += md->mpd.produced;

	audio_stream_produce(&local_buff->stream, md->mpd.produced);
	comp_update_buffer_consume(source, md->mpd.consumed);

db_verify:
	if (!produced && !mod->deep_buff_bytes) {
		comp_dbg(dev, "codec_adapter_copy(): nothing processed in this call");
		/* we haven't produced anything in this period but we
		 * still have data in the local buffer to copy to sink
		 */
		if (audio_stream_get_avail_bytes(&local_buff->stream) >= mod->period_bytes)
			goto copy_period;

		goto end;
	}

	if (mod->deep_buff_bytes) {
		if (mod->deep_buff_bytes >= audio_stream_get_avail_bytes(&local_buff->stream)) {
			generate_zeroes(sink, mod->period_bytes);
			goto end;
		}

		comp_dbg(dev, "codec_adapter_copy(): deep buffering has ended after gathering %d bytes of processed data",
			 audio_stream_get_avail_bytes(&local_buff->stream));
		mod->deep_buff_bytes = 0;
	}

copy_period:
	comp_get_copy_limits_with_lock(local_buff, sink, &cl);
	copy_bytes = cl.frames * cl.source_frame_bytes;
	audio_stream_copy(&local_buff->stream, 0,
			  &sink->stream, 0,
			  copy_bytes / mod->stream_params.sample_container_bytes);
	buffer_stream_writeback(sink, copy_bytes);

	comp_update_buffer_produce(sink, copy_bytes);
	comp_update_buffer_consume(local_buff, copy_bytes);
end:
	comp_dbg(dev, "codec_adapter_copy(): processed %d in this call %d bytes left for next period",
		 processed, bytes_to_process);

out_free:
	for (i = 0; i < mod->num_output_buffers; i++)
		rfree(output_buffers[i].data);

	rfree(output_buffers);

free:
	for (i = 0; i < mod->num_input_buffers; i++)
		rfree(input_buffers[i].data);
	rfree(input_buffers);

	return ret;
}

static int codec_adapter_set_params(struct comp_dev *dev, struct sof_ipc_ctrl_data *cdata)
{
	struct processing_module *mod = comp_get_drvdata(dev);
	struct module_data *md = &mod->priv;
	enum module_cfg_fragment_position pos;
	uint32_t data_offset_size;
	static uint32_t size;

	comp_dbg(dev, "codec_adapter_set_params(): num_of_elem %d, elem remain %d msg_index %u",
		 cdata->num_elems, cdata->elems_remaining, cdata->msg_index);

	/* set the fragment position, data offset and config data size */
	if (!cdata->msg_index) {
		size = cdata->num_elems + cdata->elems_remaining;
		data_offset_size = size;
		if (cdata->elems_remaining)
			pos = MODULE_CFG_FRAGMENT_FIRST;
		else
			pos = MODULE_CFG_FRAGMENT_SINGLE;
	} else {
		data_offset_size = size - (cdata->num_elems + cdata->elems_remaining);
		if (cdata->elems_remaining)
			pos = MODULE_CFG_FRAGMENT_MIDDLE;
		else
			pos = MODULE_CFG_FRAGMENT_LAST;
	}

	/* IPC3 does not use config_id, so pass 0 for config ID as it will be ignored anyway */
	if (md->ops->set_configuration)
		return md->ops->set_configuration(mod, 0, pos, data_offset_size,
						  (const uint8_t *)cdata->data->data,
						  cdata->num_elems, NULL, 0);

	comp_warn(dev, "codec_adapter_set_params(): no set_configuration op set for %d",
		  dev_comp_id(dev));
	return 0;
}

static int codec_adapter_ctrl_set_data(struct comp_dev *dev,
				       struct sof_ipc_ctrl_data *cdata)
{
	int ret;
	struct processing_module *mod = comp_get_drvdata(dev);

	comp_dbg(dev, "codec_adapter_ctrl_set_data() start, state %d, cmd %d",
		 mod->priv.state, cdata->cmd);

	/* Check version from ABI header */
	if (SOF_ABI_VERSION_INCOMPATIBLE(SOF_ABI_VERSION, cdata->data->abi)) {
		comp_err(dev, "codec_adapter_ctrl_set_data(): ABI mismatch!");
		return -EINVAL;
	}

	switch (cdata->cmd) {
	case SOF_CTRL_CMD_ENUM:
		comp_err(dev, "codec_adapter_ctrl_set_data() set enum is not implemented for codec_adapter.");
		ret = -EIO;
		break;
	case SOF_CTRL_CMD_BINARY:
		ret = codec_adapter_set_params(dev, cdata);
		break;
	default:
		comp_err(dev, "codec_adapter_ctrl_set_data error: unknown set data command");
		ret = -EINVAL;
		break;
	}

	return ret;
}

/* Used to pass standard and bespoke commands (with data) to component */
int codec_adapter_cmd(struct comp_dev *dev, int cmd, void *data,
		      int max_data_size)
{
	int ret;
	struct sof_ipc_ctrl_data *cdata = ASSUME_ALIGNED(data, 4);

	comp_dbg(dev, "codec_adapter_cmd() %d start", cmd);

	switch (cmd) {
	case COMP_CMD_SET_DATA:
		ret = codec_adapter_ctrl_set_data(dev, cdata);
		break;
	case COMP_CMD_GET_DATA:
		comp_err(dev, "codec_adapter_cmd() get_data not implemented yet.");
		ret = -ENODATA;
		break;
	default:
		comp_err(dev, "codec_adapter_cmd() error: unknown command");
		ret = -EINVAL;
		break;
	}

	comp_dbg(dev, "codec_adapter_cmd() done");
	return ret;
}

int codec_adapter_trigger(struct comp_dev *dev, int cmd)
{
	comp_dbg(dev, "codec_adapter_trigger(): component got trigger cmd %x", cmd);

	return comp_set_state(dev, cmd);
}

int codec_adapter_reset(struct comp_dev *dev)
{
	int ret;
	struct processing_module *mod = comp_get_drvdata(dev);

	comp_dbg(dev, "codec_adapter_reset(): resetting");

	ret = module_reset(mod);
	if (ret) {
		comp_err(dev, "codec_adapter_reset(): error %d, codec reset has failed",
			 ret);
	}

	/* if module is not prepared, local_buffer won't be allocated */
	if (mod->local_buff)
		buffer_zero(mod->local_buff);

	comp_dbg(dev, "codec_adapter_reset(): done");

	return comp_set_state(dev, COMP_TRIGGER_RESET);
}

void codec_adapter_free(struct comp_dev *dev)
{
	int ret;
	struct processing_module *mod = comp_get_drvdata(dev);

	comp_dbg(dev, "codec_adapter_free(): start");

	ret = module_free(mod);
	if (ret)
		comp_err(dev, "codec_adapter_free(): error %d, codec free failed", ret);

	buffer_free(mod->local_buff);
	rfree(mod);
	rfree(dev);
}

/* Spa ALSA Sink
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stddef.h>

#include <asoundlib.h>

#include <spa/node.h>
#include <spa/audio/format.h>

typedef struct _SpaALSASink SpaALSASink;

static const char default_device[] = "default";
static const uint32_t default_buffer_time = 10000;
static const uint32_t default_period_time = 5000;
static const bool default_period_event = 0;

typedef struct {
  SpaProps props;
  char device[64];
  char device_name[128];
  char card_name[128];
  uint32_t buffer_time;
  uint32_t period_time;
  bool period_event;
} SpaALSASinkProps;

static void
reset_alsa_sink_props (SpaALSASinkProps *props)
{
  strncpy (props->device, default_device, 64);
  props->buffer_time = default_buffer_time;
  props->period_time = default_period_time;
  props->period_event = default_period_event;
}

typedef struct {
  bool opened;
  bool have_buffers;
  snd_pcm_t *handle;
  snd_output_t *output;
  snd_pcm_sframes_t buffer_size;
  snd_pcm_sframes_t period_size;
  snd_pcm_channel_area_t areas[16];
  bool running;
  SpaPollFd fds[16];
  SpaPollItem poll;
} SpaALSAState;

typedef struct _ALSABuffer ALSABuffer;

struct _ALSABuffer {
  SpaBuffer buffer;
  SpaMeta metas[2];
  SpaMetaHeader header;
  SpaMetaRingbuffer ringbuffer;
  SpaData datas[1];
};

struct _SpaALSASink {
  SpaHandle handle;
  SpaNode node;

  SpaALSASinkProps props[2];

  SpaEventCallback event_cb;
  void *user_data;

  bool have_format;
  SpaFormatAudio query_format;
  SpaFormatAudio current_format;

  SpaALSAState state;

  SpaPortInfo info;
  SpaAllocParam *params[1];
  SpaAllocParamBuffers param_buffers;
  SpaPortStatus status;

  SpaBuffer *buffers;
  unsigned int n_buffers;
  uint32_t input_buffer;

  SpaMemory *mem;
  ALSABuffer buffer;
};

#include "alsa-utils.c"

static const uint32_t min_uint32 = 1;
static const uint32_t max_uint32 = UINT32_MAX;

static const SpaPropRangeInfo uint32_range[] = {
  { "min", "Minimum value", 4, &min_uint32 },
  { "max", "Maximum value", 4, &max_uint32 },
};

enum {
  PROP_ID_DEVICE,
  PROP_ID_DEVICE_NAME,
  PROP_ID_CARD_NAME,
  PROP_ID_BUFFER_TIME,
  PROP_ID_PERIOD_TIME,
  PROP_ID_PERIOD_EVENT,
  PROP_ID_LAST,
};

static const SpaPropInfo prop_info[] =
{
  { PROP_ID_DEVICE,             offsetof (SpaALSASinkProps, device),
                                "device", "ALSA device, as defined in an asound configuration file",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_STRING, 63,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
  { PROP_ID_DEVICE_NAME,        offsetof (SpaALSASinkProps, device_name),
                                "device-name", "Human-readable name of the sound device",
                                SPA_PROP_FLAG_READABLE,
                                SPA_PROP_TYPE_STRING, 127,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
  { PROP_ID_CARD_NAME,          offsetof (SpaALSASinkProps, card_name),
                                "card-name", "Human-readable name of the sound card",
                                SPA_PROP_FLAG_READABLE,
                                SPA_PROP_TYPE_STRING, 127,
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
  { PROP_ID_BUFFER_TIME,        offsetof (SpaALSASinkProps, buffer_time),
                                "buffer-time", "The total size of the buffer in time",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                SPA_PROP_RANGE_TYPE_MIN_MAX, 2, uint32_range,
                                NULL },
  { PROP_ID_PERIOD_TIME,        offsetof (SpaALSASinkProps, period_time),
                                "period-time", "The size of a period in time",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                SPA_PROP_RANGE_TYPE_MIN_MAX, 2, uint32_range,
                                NULL },
  { PROP_ID_PERIOD_EVENT,       offsetof (SpaALSASinkProps, period_event),
                                "period-event", "Generate an event each period",
                                SPA_PROP_FLAG_READWRITE,
                                SPA_PROP_TYPE_BOOL, sizeof (bool),
                                SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                NULL },
};

static SpaResult
spa_alsa_sink_node_get_props (SpaNode       *node,
                              SpaProps     **props)
{
  SpaALSASink *this;

  if (node == NULL || node->handle == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASink *) node->handle;

  memcpy (&this->props[0], &this->props[1], sizeof (this->props[1]));
  *props = &this->props[0].props;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_set_props (SpaNode         *node,
                              const SpaProps  *props)
{
  SpaALSASink *this;
  SpaALSASinkProps *p;
  SpaResult res;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASink *) node->handle;
  p = &this->props[1];

  if (props == NULL) {
    reset_alsa_sink_props (p);
    return SPA_RESULT_OK;
  }

  res = spa_props_copy (props, &p->props);

  return res;
}

static SpaResult
spa_alsa_sink_node_send_command (SpaNode       *node,
                                 SpaCommand    *command)
{
  SpaALSASink *this;

  if (node == NULL || node->handle == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASink *) node->handle;

  switch (command->type) {
    case SPA_COMMAND_INVALID:
      return SPA_RESULT_INVALID_COMMAND;

    case SPA_COMMAND_START:
      spa_alsa_start (this);

      if (this->event_cb) {
        SpaEvent event;
        SpaEventStateChange sc;

        event.type = SPA_EVENT_TYPE_STATE_CHANGE;
        event.port_id = -1;
        event.data = &sc;
        event.size = sizeof (sc);
        sc.state = SPA_NODE_STATE_STREAMING;

        this->event_cb (node, &event, this->user_data);
      }
      break;
    case SPA_COMMAND_PAUSE:
      spa_alsa_stop (this);

      if (this->event_cb) {
        SpaEvent event;
        SpaEventStateChange sc;

        event.type = SPA_EVENT_TYPE_STATE_CHANGE;
        event.port_id = -1;
        event.data = &sc;
        event.size = sizeof (sc);
        sc.state = SPA_NODE_STATE_PAUSED;

        this->event_cb (node, &event, this->user_data);
      }
      break;
    case SPA_COMMAND_FLUSH:
    case SPA_COMMAND_DRAIN:
    case SPA_COMMAND_MARKER:
      return SPA_RESULT_NOT_IMPLEMENTED;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_set_event_callback (SpaNode          *node,
                                       SpaEventCallback  event,
                                       void             *user_data)
{
  SpaALSASink *this;

  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASink *) node->handle;

  this->event_cb = event;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_get_n_ports (SpaNode       *node,
                                unsigned int  *n_input_ports,
                                unsigned int  *max_input_ports,
                                unsigned int  *n_output_ports,
                                unsigned int  *max_output_ports)
{
  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports)
    *n_input_ports = 1;
  if (n_output_ports)
    *n_output_ports = 0;
  if (max_input_ports)
    *max_input_ports = 1;
  if (max_output_ports)
    *max_output_ports = 0;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_get_port_ids (SpaNode       *node,
                                 unsigned int   n_input_ports,
                                 uint32_t      *input_ids,
                                 unsigned int   n_output_ports,
                                 uint32_t      *output_ids)
{
  if (node == NULL || node->handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports > 0)
    input_ids[0] = 0;

  return SPA_RESULT_OK;
}


static SpaResult
spa_alsa_sink_node_add_port (SpaNode        *node,
                             SpaDirection    direction,
                             uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_sink_node_remove_port (SpaNode        *node,
                                uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_sink_node_port_enum_formats (SpaNode         *node,
                                      uint32_t         port_id,
                                      SpaFormat      **format,
                                      const SpaFormat *filter,
                                      void           **state)
{
  SpaALSASink *this;
  int index;

  if (node == NULL || node->handle == NULL || format == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASink *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      spa_format_audio_init (SPA_MEDIA_TYPE_AUDIO,
                             SPA_MEDIA_SUBTYPE_RAW,
                             &this->query_format);
      break;
    case 1:
      spa_format_audio_init (SPA_MEDIA_TYPE_AUDIO,
                             SPA_MEDIA_SUBTYPE_AAC,
                             &this->query_format);
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = &this->query_format.format;
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_port_set_format (SpaNode            *node,
                                    uint32_t            port_id,
                                    SpaPortFormatFlags  flags,
                                    const SpaFormat    *format)
{
  SpaALSASink *this;
  SpaResult res;

  if (node == NULL || node->handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASink *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (format == NULL) {
    this->have_format = false;
    return SPA_RESULT_OK;
  }

  if ((res = spa_format_audio_parse (format, &this->current_format)) < 0)
    return res;

  if (alsa_set_format (this, &this->current_format, false) < 0)
    return SPA_RESULT_ERROR;

  this->info.flags = SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS;
  this->info.maxbuffering = -1;
  this->info.latency = -1;
  this->info.n_params = 1;
  this->params[0] = &this->param_buffers.param;
  this->param_buffers.param.type = SPA_ALLOC_PARAM_TYPE_BUFFERS;
  this->param_buffers.param.size = sizeof (this->param_buffers);
  this->param_buffers.minsize = 1;
  this->param_buffers.stride = 0;
  this->param_buffers.min_buffers = 1;
  this->param_buffers.max_buffers = 8;
  this->param_buffers.align = 16;
  this->info.features = NULL;

  this->have_format = true;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_port_get_format (SpaNode          *node,
                                    uint32_t          port_id,
                                    const SpaFormat **format)
{
  SpaALSASink *this;

  if (node == NULL || node->handle == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASink *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = &this->current_format.format;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_port_get_info (SpaNode            *node,
                                  uint32_t            port_id,
                                  const SpaPortInfo **info)
{
  SpaALSASink *this;

  if (node == NULL || node->handle == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASink *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  *info = &this->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_port_get_props (SpaNode    *node,
                                   uint32_t    port_id,
                                   SpaProps  **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_sink_node_port_set_props (SpaNode         *node,
                                   uint32_t         port_id,
                                   const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_sink_node_port_use_buffers (SpaNode         *node,
                                     uint32_t         port_id,
                                     SpaBuffer      **buffers,
                                     uint32_t         n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_sink_node_port_alloc_buffers (SpaNode         *node,
                                       uint32_t         port_id,
                                       SpaAllocParam  **params,
                                       uint32_t         n_params,
                                       SpaBuffer      **buffers,
                                       uint32_t        *n_buffers)
{
  SpaALSASink *this;
  ALSABuffer *b;
  SpaALSAState *state;

  if (node == NULL || node->handle == NULL || buffers == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  this = (SpaALSASink *) node->handle;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  state = &this->state;

  if (!this->mem)
    this->mem = spa_memory_alloc_with_fd (SPA_MEMORY_POOL_SHARED, NULL, state->buffer_size);

  b = &this->buffer;
  b->buffer.id = 0;
  b->buffer.mem.mem.pool_id = -1;
  b->buffer.mem.mem.id = -1;
  b->buffer.mem.offset = 0;
  b->buffer.mem.size = sizeof (ALSABuffer);

  b->buffer.n_metas = 2;
  b->buffer.metas = offsetof (ALSABuffer, metas);
  b->buffer.n_datas = 1;
  b->buffer.datas = offsetof (ALSABuffer, datas);

  b->header.flags = 0;
  b->header.seq = 0;
  b->header.pts = 0;
  b->header.dts_offset = 0;

  b->metas[0].type = SPA_META_TYPE_HEADER;
  b->metas[0].offset = offsetof (ALSABuffer, header);
  b->metas[0].size = sizeof (b->header);

  b->ringbuffer.readindex = 0;
  b->ringbuffer.writeindex = 0;
  b->ringbuffer.size = 0;
  b->ringbuffer.size_mask = 0;

  b->metas[1].type = SPA_META_TYPE_RINGBUFFER;
  b->metas[1].offset = offsetof (ALSABuffer, ringbuffer);
  b->metas[1].size = sizeof (b->ringbuffer);

  b->datas[0].mem.mem = this->mem->mem;
  b->datas[0].mem.offset = 0;
  b->datas[0].mem.size = state->buffer_size;
  b->datas[0].stride = 0;

  buffers[0] = &b->buffer;
  *n_buffers = 1;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_port_reuse_buffer (SpaNode         *node,
                                      uint32_t         port_id,
                                      uint32_t         buffer_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_alsa_sink_node_port_get_status (SpaNode              *node,
                                    uint32_t              port_id,
                                    const SpaPortStatus **status)
{
  SpaALSASink *this;

  if (node == NULL || node->handle == NULL || status == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASink *) node->handle;

  if (port_id != 0)
    return SPA_RESULT_INVALID_PORT;

  *status = &this->status;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_port_push_input (SpaNode        *node,
                                    unsigned int    n_info,
                                    SpaInputInfo   *info)
{
  SpaALSASink *this;
  unsigned int i;
  bool have_error = false, have_enough = false;

  if (node == NULL || node->handle == NULL || n_info == 0 || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASink *) node->handle;

  for (i = 0; i < n_info; i++) {
    if (info[i].port_id != 0) {
      info[i].status = SPA_RESULT_INVALID_PORT;
      have_error = true;
      continue;
    }

    if (info[i].buffer_id != SPA_ID_INVALID) {
      if (!this->have_format) {
        info[i].status = SPA_RESULT_NO_FORMAT;
        have_error = true;
        continue;
      }

      if (this->input_buffer != -1) {
        info[i].status = SPA_RESULT_HAVE_ENOUGH_INPUT;
        have_enough = true;
        continue;
      }
      this->input_buffer = info[i].buffer_id;
    }
    info[i].status = SPA_RESULT_OK;
  }
  if (have_error)
    return SPA_RESULT_ERROR;
  if (have_enough)
    return SPA_RESULT_HAVE_ENOUGH_INPUT;

  return SPA_RESULT_OK;
}

static SpaResult
spa_alsa_sink_node_port_pull_output (SpaNode        *node,
                                     unsigned int    n_info,
                                     SpaOutputInfo  *info)
{
  return SPA_RESULT_INVALID_PORT;
}

static const SpaNode alsasink_node = {
  NULL,
  sizeof (SpaNode),
  spa_alsa_sink_node_get_props,
  spa_alsa_sink_node_set_props,
  spa_alsa_sink_node_send_command,
  spa_alsa_sink_node_set_event_callback,
  spa_alsa_sink_node_get_n_ports,
  spa_alsa_sink_node_get_port_ids,
  spa_alsa_sink_node_add_port,
  spa_alsa_sink_node_remove_port,
  spa_alsa_sink_node_port_enum_formats,
  spa_alsa_sink_node_port_set_format,
  spa_alsa_sink_node_port_get_format,
  spa_alsa_sink_node_port_get_info,
  spa_alsa_sink_node_port_get_props,
  spa_alsa_sink_node_port_set_props,
  spa_alsa_sink_node_port_use_buffers,
  spa_alsa_sink_node_port_alloc_buffers,
  spa_alsa_sink_node_port_reuse_buffer,
  spa_alsa_sink_node_port_get_status,
  spa_alsa_sink_node_port_push_input,
  spa_alsa_sink_node_port_pull_output,
};

static SpaResult
spa_alsa_sink_get_interface (SpaHandle               *handle,
                             uint32_t                 interface_id,
                             void                   **interface)
{
  SpaALSASink *this;

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaALSASink *) handle;

  switch (interface_id) {
    case SPA_INTERFACE_ID_NODE:
      *interface = &this->node;
      break;
    default:
      return SPA_RESULT_UNKNOWN_INTERFACE;
  }
  return SPA_RESULT_OK;
}

static SpaResult
alsa_sink_init (const SpaHandleFactory  *factory,
                SpaHandle               *handle)
{
  SpaALSASink *this;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_alsa_sink_get_interface;

  this = (SpaALSASink *) handle;
  this->node = alsasink_node;
  this->node.handle = handle;
  this->props[1].props.n_prop_info = PROP_ID_LAST;
  this->props[1].props.prop_info = prop_info;
  reset_alsa_sink_props (&this->props[1]);

  this->status.flags = SPA_PORT_STATUS_FLAG_NEED_INPUT;

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo alsa_sink_interfaces[] =
{
  { SPA_INTERFACE_ID_NODE,
    SPA_INTERFACE_ID_NODE_NAME,
    SPA_INTERFACE_ID_NODE_DESCRIPTION,
  },
};

static SpaResult
alsa_sink_enum_interface_info (const SpaHandleFactory  *factory,
                               const SpaInterfaceInfo **info,
                               void                   **state)
{
  int index;

  if (factory == NULL || info == NULL || state == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  index = (*state == NULL ? 0 : *(int*)state);

  switch (index) {
    case 0:
      *info = &alsa_sink_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *(int*)state = ++index;

  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_alsa_sink_factory =
{ "alsa-sink",
  NULL,
  sizeof (SpaALSASink),
  alsa_sink_init,
  alsa_sink_enum_interface_info,
};

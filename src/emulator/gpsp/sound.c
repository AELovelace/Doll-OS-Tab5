/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */


#include "common.h"
#ifdef RETRO_GO
#include "rg_system.h"
#endif

direct_sound_struct direct_sound_channel[2];
gbc_sound_struct gbc_sound_channel[4];

const u32 sound_frequency = GBA_SOUND_FREQUENCY;

u32 sound_on;
#ifdef RETRO_GO
typedef struct
{
  s16 sound_buffer[BUFFER_SIZE];
  u32 noise_table15[1024];
} gba_sound_scratch_t;

static gba_sound_scratch_t *gba_sound_scratch;
#define sound_buffer (gba_sound_scratch->sound_buffer)
#define noise_table15 (gba_sound_scratch->noise_table15)

bool gba_sound_scratch_init(void)
{
  if(gba_sound_scratch)
    return true;
  gba_sound_scratch = (gba_sound_scratch_t *)rg_alloc(
      sizeof(*gba_sound_scratch), MEM_SLOW | MEM_8BIT | MEM_NOPANIC);
  if(gba_sound_scratch)
    memset(gba_sound_scratch, 0, sizeof(*gba_sound_scratch));
  return gba_sound_scratch != NULL;
}

void gba_sound_scratch_term(void)
{
  if(gba_sound_scratch)
    heap_caps_free(gba_sound_scratch);
  gba_sound_scratch = NULL;
}
#else
static s16 sound_buffer[BUFFER_SIZE];
#endif
u32 sound_buffer_base;
u32 sound_timer_calls[2];
u32 sound_fifo_queue_words[2];
u32 sound_fifo_queue_nonzero_bytes[2];
u32 sound_timer_nonzero_samples[2];
u32 sound_timer_peak_samples[2];
u32 sound_read_calls;
u32 sound_samples_requested;
u32 sound_samples_returned;
u32 sound_last_samples_available;
u32 sound_max_samples_available;
u32 sound_stale_samples_dropped;
u32 sound_drop_events;
u32 sound_last_drop_samples;
u32 sound_max_drop_samples;
u32 sound_underrun_samples;
u32 sound_underrun_events;
u32 sound_last_underrun_samples;
u32 sound_clip_samples;
u32 sound_low_level_samples;
u32 sound_nonzero_samples;
u32 sound_peak_sample;
u32 sound_jump_samples;
u32 sound_boundary_jumps;
u32 sound_max_jump;
u32 sound_last_jump;
u32 sound_fifo_empty_reads;
u32 sound_fifo_short_reads;
u32 sound_fifo_reset_count;
u32 sound_fifo_reset_realign_count;

static fixed16_16 gbc_sound_tick_step;

#ifdef RETRO_GO
#define SOUND_JUMP_THRESHOLD 4096
static s16 sound_last_output_sample[2];
static u8 sound_have_last_output_sample[2];
#endif

/* Queue 4 samples to the top of the DS FIFO, wrap around circularly */

void sound_timer_queue32(u32 channel, u32 value)
{
  direct_sound_struct *ds = &direct_sound_channel[channel];

  sound_fifo_queue_words[channel]++;
  for(u32 shift = 0; shift < 32; shift += 8)
  {
    if(((value >> shift) & 0xFF) != 0)
      sound_fifo_queue_nonzero_bytes[channel]++;
  }

  ds->fifo[ds->fifo_top++] = value & 0xFF;
  ds->fifo_top &= 31;

  ds->fifo[ds->fifo_top++] = (value >> 8) & 0xFF;
  ds->fifo_top &= 31;

  ds->fifo[ds->fifo_top++] = (value >> 16) & 0xFF;
  ds->fifo_top &= 31;

  ds->fifo[ds->fifo_top++] = (value >> 24);
  ds->fifo_top &= 31;
}


unsigned sound_timer(fixed8_24 frequency_step, u32 channel)
{
#if defined(GBA_DISABLE_SOUND_RENDER) && GBA_DISABLE_SOUND_RENDER
  (void)frequency_step;
  (void)channel;
  return 0;
#else
  int ret = 0;
  u32 sample_status = DIRECT_SOUND_INACTIVE;
  direct_sound_struct *ds = &direct_sound_channel[channel];
  sound_timer_calls[channel]++;

  fixed8_24 fifo_fractional = ds->fifo_fractional;
  u32 buffer_index = ds->buffer_index;
  s16 current_sample, next_sample;

  current_sample = ((s8)ds->fifo[ds->fifo_base]) * 16;
  if(current_sample != 0)
  {
    u32 magnitude = current_sample < 0 ? (u32)(-current_sample) : (u32)current_sample;
    sound_timer_nonzero_samples[channel]++;
    if(magnitude > sound_timer_peak_samples[channel])
      sound_timer_peak_samples[channel] = magnitude;
  }
  ds->fifo_base = (ds->fifo_base + 1) % 32;
  next_sample = ((s8)ds->fifo[ds->fifo_base]) * 16;

  if(sound_on == 1)
  {
    current_sample >>= ds->volume_halve;
    next_sample >>= ds->volume_halve;

    sample_status = ds->status;
  }

  // Unqueue 1 sample from the base of the DS FIFO and place it on the audio
  // buffer for as many samples as necessary. If the DS FIFO is 16 bytes or
  // smaller and if DMA is enabled for the sound channel initiate a DMA transfer
  // to the DS FIFO.

  switch(sample_status)
  {
     case DIRECT_SOUND_INACTIVE:
        /* render samples NULL */
        while(fifo_fractional <= 0xFFFFFF)
        {
           fifo_fractional += frequency_step;
           buffer_index = (buffer_index + 2) % BUFFER_SIZE;
        }
        break;

     case DIRECT_SOUND_RIGHT:
        /* render samples RIGHT */
        while(fifo_fractional <= 0xFFFFFF)
        {
           s16 dest_sample = current_sample +
              fp16_16_to_u32((next_sample - current_sample) * (fifo_fractional >> 8));

           sound_buffer[buffer_index + 1]     += dest_sample;

           fifo_fractional += frequency_step;
           buffer_index = (buffer_index + 2) % BUFFER_SIZE;
        }
        break;

     case DIRECT_SOUND_LEFT:
        /* render samples LEFT */
        while(fifo_fractional <= 0xFFFFFF)
        {
           s16 dest_sample = current_sample +
              fp16_16_to_u32((next_sample - current_sample) * (fifo_fractional >> 8));

           sound_buffer[buffer_index]     += dest_sample;

           fifo_fractional += frequency_step;
           buffer_index = (buffer_index + 2) % BUFFER_SIZE;
        }
        break;

     case DIRECT_SOUND_LEFTRIGHT:
        /* render samples LEFT and RIGHT. */
        while(fifo_fractional <= 0xFFFFFF)
        {
           s16 dest_sample = current_sample +
              fp16_16_to_u32((next_sample - current_sample) * (fifo_fractional >> 8));

           sound_buffer[buffer_index]     += dest_sample;
           sound_buffer[buffer_index + 1] += dest_sample;
           fifo_fractional += frequency_step;
           buffer_index = (buffer_index + 2) % BUFFER_SIZE;
        }
        break;
  }

  ds->buffer_index = buffer_index;
  ds->fifo_fractional = fp8_24_fractional_part(fifo_fractional);

  if(((ds->fifo_top - ds->fifo_base) % 32) <= 16)
  {
    if(dma[1].direct_sound_channel == channel)
      dma_transfer(1, &ret);

    if(dma[2].direct_sound_channel == channel)
      dma_transfer(2, &ret);
  }
  return ret;
#endif
}

void sound_reset_fifo(u32 channel)
{
  direct_sound_struct *ds = &direct_sound_channel[channel];

  sound_fifo_reset_count++;
  if(ds->fifo_base || ds->fifo_top || ds->fifo_fractional)
     sound_fifo_reset_realign_count++;

  memset(ds->fifo, 0, 32);
  ds->fifo_base = 0;
  ds->fifo_top = 0;
  ds->fifo_fractional = 0;
}

// Initial pattern data = 4bits (signed)
// Channel volume = 12bits
// Envelope volume = 14bits
// Master volume = 2bits

// Recalculate left and right volume as volume changes.
// To calculate the current sample, use (sample * volume) >> 16

// Square waves range from -8 (low) to 7 (high)

const s8 square_pattern_duty[4][8] =
{
  { -8, -8, -8, -8,  7, -8, -8, -8 },
  { -8, -8, -8, -8,  7,  7, -8, -8 },
  { -8, -8,  7,  7,  7,  7, -8, -8 },
  {  7,  7,  7,  7, -8, -8,  7,  7 },
};

s8 wave_samples[64];

#ifndef RETRO_GO
u32 noise_table15[1024];
#endif
u32 noise_table7[4];

const u32 gbc_sound_master_volume_table[4] = { 1, 2, 4, 0 };

const u32 gbc_sound_channel_volume_table[8] =
{
  fixed_div(0, 7, 12),
  fixed_div(1, 7, 12),
  fixed_div(2, 7, 12),
  fixed_div(3, 7, 12),
  fixed_div(4, 7, 12),
  fixed_div(5, 7, 12),
  fixed_div(6, 7, 12),
  fixed_div(7, 7, 12)
};

const u32 gbc_sound_envelope_volume_table[16] =
{
  fixed_div(0, 15, 14),
  fixed_div(1, 15, 14),
  fixed_div(2, 15, 14),
  fixed_div(3, 15, 14),
  fixed_div(4, 15, 14),
  fixed_div(5, 15, 14),
  fixed_div(6, 15, 14),
  fixed_div(7, 15, 14),
  fixed_div(8, 15, 14),
  fixed_div(9, 15, 14),
  fixed_div(10, 15, 14),
  fixed_div(11, 15, 14),
  fixed_div(12, 15, 14),
  fixed_div(13, 15, 14),
  fixed_div(14, 15, 14),
  fixed_div(15, 15, 14)
};

u32 gbc_sound_buffer_index = 0;
u32 gbc_sound_last_cpu_ticks = 0;
u32 gbc_sound_partial_ticks = 0;

u32 gbc_sound_master_volume_left;
u32 gbc_sound_master_volume_right;
u32 gbc_sound_master_volume;

#define update_volume_channel_envelope(channel)                               \
  volume_##channel = gbc_sound_envelope_volume_table[envelope_volume] *       \
   gbc_sound_channel_volume_table[gbc_sound_master_volume_##channel] *        \
   gbc_sound_master_volume_table[gbc_sound_master_volume]                     \

#define update_volume_channel_noenvelope(channel)                             \
  volume_##channel = gs->wave_volume *                                        \
   gbc_sound_channel_volume_table[gbc_sound_master_volume_##channel] *        \
   gbc_sound_master_volume_table[gbc_sound_master_volume]                     \

#define update_volume(type)                                                   \
  update_volume_channel_##type(left);                                         \
  update_volume_channel_##type(right)                                         \

#define update_tone_sweep()                                                   \
  if(gs->sweep_status)                                                        \
  {                                                                           \
    u32 sweep_ticks = gs->sweep_ticks - 1;                                    \
                                                                              \
    if(sweep_ticks == 0)                                                      \
    {                                                                         \
      u32 rate = gs->rate;                                                    \
                                                                              \
      if(gs->sweep_direction)                                                 \
        rate = rate - (rate >> gs->sweep_shift);                              \
      else                                                                    \
        rate = rate + (rate >> gs->sweep_shift);                              \
                                                                              \
      if(rate > 2047) {                                                       \
        rate = 2047;                                                          \
        gs->active_flag = 0;                                                  \
        break;                                                                \
      }                                                                       \
                                                                              \
      frequency_step = float_to_fp16_16(((131072.0f / (2048 - rate)) * 8.0f)  \
       / sound_frequency);                                                    \
                                                                              \
      gs->frequency_step = frequency_step;                                    \
      gs->rate = rate;                                                        \
                                                                              \
      sweep_ticks = gs->sweep_initial_ticks;                                  \
    }                                                                         \
    gs->sweep_ticks = sweep_ticks;                                            \
  }                                                                           \

#define update_tone_nosweep()                                                 \

#define update_tone_envelope()                                                \
  if(gs->envelope_status)                                                     \
  {                                                                           \
    u32 envelope_ticks = gs->envelope_ticks - 1;                              \
    envelope_volume = gs->envelope_volume;                                    \
                                                                              \
    if(envelope_ticks == 0)                                                   \
    {                                                                         \
      if(gs->envelope_direction)                                              \
      {                                                                       \
        if(envelope_volume != 15)                                             \
          envelope_volume = gs->envelope_volume + 1;                          \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        if(envelope_volume != 0)                                              \
          envelope_volume = gs->envelope_volume - 1;                          \
      }                                                                       \
                                                                              \
      update_volume(envelope);                                                \
                                                                              \
      gs->envelope_volume = envelope_volume;                                  \
      gs->envelope_ticks = gs->envelope_initial_ticks;                        \
    }                                                                         \
    else                                                                      \
      gs->envelope_ticks = envelope_ticks;                                    \
  }                                                                           \

#define update_tone_noenvelope()                                              \

#define update_tone_counters(envelope_op, sweep_op)                           \
  tick_counter += gbc_sound_tick_step;                                        \
  if(tick_counter > 0xFFFF)                                                   \
  {                                                                           \
    if(gs->length_status)                                                     \
    {                                                                         \
      u32 length_ticks = gs->length_ticks - 1;                                \
      gs->length_ticks = length_ticks;                                        \
                                                                              \
      if(length_ticks == 0)                                                   \
      {                                                                       \
        gs->active_flag = 0;                                                  \
        break;                                                                \
      }                                                                       \
    }                                                                         \
                                                                              \
    update_tone_##envelope_op();                                              \
    update_tone_##sweep_op();                                                 \
                                                                              \
    tick_counter &= 0xFFFF;                                                   \
  }                                                                           \

#define gbc_sound_render_sample_right()                                       \
  sound_buffer[buffer_index + 1] += (current_sample * volume_right) >> 22     \

#define gbc_sound_render_sample_left()                                        \
  sound_buffer[buffer_index] += (current_sample * volume_left) >> 22          \

#define gbc_sound_render_sample_both()                                        \
  gbc_sound_render_sample_right();                                            \
  gbc_sound_render_sample_left()                                              \

#define gbc_sound_render_samples(type, sample_length, envelope_op, sweep_op)  \
  for(i = 0; i < buffer_ticks; i++)                                           \
  {                                                                           \
    current_sample =                                                          \
     sample_data[fp16_16_to_u32(sample_index) % sample_length];               \
    gbc_sound_render_sample_##type();                                         \
                                                                              \
    sample_index += frequency_step;                                           \
    buffer_index = (buffer_index + 2) % BUFFER_SIZE;                          \
                                                                              \
    update_tone_counters(envelope_op, sweep_op);                              \
  }                                                                           \

#define gbc_noise_wrap_full 32767

#define gbc_noise_wrap_half 126

#define get_noise_sample_full()                                               \
  current_sample =                                                            \
   ((s32)(noise_table15[fp16_16_to_u32(sample_index) >> 5] <<                 \
   (fp16_16_to_u32(sample_index) & 0x1F)) >> 31) ^ 0x07                       \

#define get_noise_sample_half()                                               \
  current_sample =                                                            \
   ((s32)(noise_table7[fp16_16_to_u32(sample_index) >> 5] <<                  \
   (fp16_16_to_u32(sample_index) & 0x1F)) >> 31) ^ 0x07                       \

#define gbc_sound_render_noise(type, noise_type, envelope_op, sweep_op)       \
  for(i = 0; i < buffer_ticks; i++)                                           \
  {                                                                           \
    get_noise_sample_##noise_type();                                          \
    gbc_sound_render_sample_##type();                                         \
                                                                              \
    sample_index += frequency_step;                                           \
                                                                              \
    if(sample_index >= u32_to_fp16_16(gbc_noise_wrap_##noise_type))           \
      sample_index -= u32_to_fp16_16(gbc_noise_wrap_##noise_type);            \
                                                                              \
    buffer_index = (buffer_index + 2) % BUFFER_SIZE;                          \
    update_tone_counters(envelope_op, sweep_op);                              \
  }                                                                           \

#define gbc_sound_render_channel(type, sample_length, envelope_op, sweep_op)  \
  buffer_index = gbc_sound_buffer_index;                                      \
  sample_index = gs->sample_index;                                            \
  frequency_step = gs->frequency_step;                                        \
  tick_counter = gs->tick_counter;                                            \
                                                                              \
  update_volume(envelope_op);                                                 \
                                                                              \
  switch(gs->status)                                                          \
  {                                                                           \
    case GBC_SOUND_INACTIVE:                                                  \
      break;                                                                  \
                                                                              \
    case GBC_SOUND_LEFT:                                                      \
      gbc_sound_render_##type(left, sample_length, envelope_op, sweep_op);    \
      break;                                                                  \
                                                                              \
    case GBC_SOUND_RIGHT:                                                     \
      gbc_sound_render_##type(right, sample_length, envelope_op, sweep_op);   \
      break;                                                                  \
                                                                              \
    case GBC_SOUND_LEFTRIGHT:                                                 \
      gbc_sound_render_##type(both, sample_length, envelope_op, sweep_op);    \
      break;                                                                  \
  }                                                                           \
                                                                              \
  gs->sample_index = sample_index;                                            \
  gs->tick_counter = tick_counter;                                            \

void render_gbc_sound()
{
#if defined(GBA_DISABLE_SOUND_RENDER) && GBA_DISABLE_SOUND_RENDER
  gbc_sound_last_cpu_ticks = cpu_ticks;
  return;
#endif

  u32 i, i2;
  gbc_sound_struct *gs = gbc_sound_channel;
  fixed16_16 sample_index, frequency_step;
  fixed16_16 tick_counter;
  u32 buffer_index;
  s32 volume_left, volume_right;
  u32 envelope_volume;
  s32 current_sample;
  u16 sound_status = read_ioreg(REG_SOUNDCNT_X) & 0xFFF0;
  const s8 *sample_data;
  u32 tick_delta = cpu_ticks - gbc_sound_last_cpu_ticks;
  fixed16_16 buffer_ticks = float_to_fp16_16((float)(tick_delta) *
                                             sound_frequency / GBC_BASE_RATE);
  if (!tick_delta)
    return;

  gbc_update_count++;
  gbc_sound_partial_ticks += fp16_16_fractional_part(buffer_ticks);
  buffer_ticks = fp16_16_to_u32(buffer_ticks);

  if(gbc_sound_partial_ticks > 0xFFFF)
  {
    buffer_ticks += 1;
    gbc_sound_partial_ticks &= 0xFFFF;
  }

  if(sound_on == 1)
  {
    s8 *wave_bank;
    gs = gbc_sound_channel + 0;
    if(gs->active_flag)
    {
      sound_status |= 0x01;
      sample_data = &square_pattern_duty[gs->sample_table_idx][0];
      envelope_volume = gs->envelope_volume;
      gbc_sound_render_channel(samples, 8, envelope, sweep);
    }

    gs = gbc_sound_channel + 1;
    if(gs->active_flag)
    {
      sound_status |= 0x02;
      sample_data = &square_pattern_duty[gs->sample_table_idx][0];
      envelope_volume = gs->envelope_volume;
      gbc_sound_render_channel(samples, 8, envelope, nosweep);
    }

    gs = gbc_sound_channel + 2;
    if(gbc_sound_wave_update)
    {
       unsigned bank = (gs->wave_bank == 1) ? 1 : 0;
       u8 *wave_ram = ((u8 *)io_registers) + 0x90;

       wave_bank = wave_samples + (bank * 32);
       for(i = 0, i2 = 0; i < 16; i++, i2 += 2)
       {
          current_sample = wave_ram[i];
          wave_bank[i2] = (((current_sample >> 4) & 0x0F) - 8);
          wave_bank[i2 + 1] = ((current_sample & 0x0F) - 8);
       }

       gbc_sound_wave_update = 0;
    }

    if((gs->active_flag) && (gs->master_enable))
    {
      sound_status |= 0x04;
      sample_data = wave_samples;
      if(gs->wave_type == 0)
      {
        if(gs->wave_bank == 1)
          sample_data += 32;

        gbc_sound_render_channel(samples, 32, noenvelope, nosweep);
      }
      else
      {
        gbc_sound_render_channel(samples, 64, noenvelope, nosweep);
      }
    }

    gs = gbc_sound_channel + 3;
    if(gs->active_flag)
    {
      sound_status |= 0x08;
      envelope_volume = gs->envelope_volume;

      if(gs->noise_type == 1)
      {
        gbc_sound_render_channel(noise, half, envelope, nosweep);
      }
      else
      {
        gbc_sound_render_channel(noise, full, envelope, nosweep);
      }
    }
  }

  write_ioreg(REG_SOUNDCNT_X, sound_status);

  gbc_sound_last_cpu_ticks = cpu_ticks;
  gbc_sound_buffer_index =
   (gbc_sound_buffer_index + (buffer_ticks * 2)) % BUFFER_SIZE;
}

// Special thanks to blarrg for the LSFR frequency used in Meridian, as posted
// on the forum at http://meridian.overclocked.org:
// http://meridian.overclocked.org/cgi-bin/wwwthreads/showpost.pl?Board=merid
// angeneraldiscussion&Number=2069&page=0&view=expanded&mode=threaded&sb=4
// Hope you don't mind me borrowing it ^_-

static void init_noise_table(u32 *table, u32 period, u32 bit_length)
{
  u32 shift_register = 0xFF;
  u32 mask = ~(1 << bit_length);
  s32 table_pos, bit_pos;
  u32 current_entry;
  u32 table_period = (period + 31) / 32;

  // Bits are stored in reverse order so they can be more easily moved to
  // bit 31, for sign extended shift down.

  for(table_pos = 0; table_pos < table_period; table_pos++)
  {
    current_entry = 0;
    for(bit_pos = 31; bit_pos >= 0; bit_pos--)
    {
      current_entry |= (shift_register & 0x01) << bit_pos;

      shift_register =
       ((1 & (shift_register ^ (shift_register >> 1))) << bit_length) |
       ((shift_register >> 1) & mask);
    }

    table[table_pos] = current_entry;
  }
}

void reset_sound(void)
{
  direct_sound_struct *ds = direct_sound_channel;
  gbc_sound_struct *gs = gbc_sound_channel;
  u32 i;

  sound_on = 0;
  sound_buffer_base = 0;
  sound_timer_calls[0] = sound_timer_calls[1] = 0;
  sound_fifo_queue_words[0] = sound_fifo_queue_words[1] = 0;
  sound_fifo_queue_nonzero_bytes[0] = sound_fifo_queue_nonzero_bytes[1] = 0;
  sound_timer_nonzero_samples[0] = sound_timer_nonzero_samples[1] = 0;
  sound_timer_peak_samples[0] = sound_timer_peak_samples[1] = 0;
  sound_read_calls = 0;
  sound_samples_requested = 0;
  sound_samples_returned = 0;
  sound_last_samples_available = 0;
  sound_max_samples_available = 0;
  sound_stale_samples_dropped = 0;
  sound_drop_events = 0;
  sound_last_drop_samples = 0;
  sound_max_drop_samples = 0;
  sound_underrun_samples = 0;
  sound_underrun_events = 0;
  sound_last_underrun_samples = 0;
  sound_clip_samples = 0;
  sound_low_level_samples = 0;
  sound_nonzero_samples = 0;
  sound_peak_sample = 0;
  sound_jump_samples = 0;
  sound_boundary_jumps = 0;
  sound_max_jump = 0;
  sound_last_jump = 0;
  sound_fifo_empty_reads = 0;
  sound_fifo_short_reads = 0;
  sound_fifo_reset_count = 0;
  sound_fifo_reset_realign_count = 0;
#ifdef RETRO_GO
  memset(sound_last_output_sample, 0, sizeof(sound_last_output_sample));
  memset(sound_have_last_output_sample, 0, sizeof(sound_have_last_output_sample));
#endif
  memset(sound_buffer, 0, sizeof(sound_buffer));

  for(i = 0; i < 2; i++, ds++)
  {
    ds->buffer_index = 0;
    ds->status = DIRECT_SOUND_INACTIVE;
    ds->fifo_top = 0;
    ds->fifo_base = 0;
    ds->fifo_fractional = 0;
    ds->volume_halve = 1;
    memset(ds->fifo, 0, 32);
  }

  gbc_sound_buffer_index = 0;
  gbc_sound_last_cpu_ticks = 0;
  gbc_sound_partial_ticks = 0;

  gbc_sound_master_volume_left = 0;
  gbc_sound_master_volume_right = 0;
  gbc_sound_master_volume = 0;
  memset(wave_samples, 0, 64);

  memset(&gbc_sound_channel[0], 0, sizeof(gbc_sound_channel));
  for(i = 0; i < 4; i++, gs++)
  {
    gs->status = GBC_SOUND_INACTIVE;
    gs->sample_table_idx = 2;
    gs->active_flag = 0;
  }
}

void init_sound()
{
  gbc_sound_tick_step =
   float_to_fp16_16(256.0f / sound_frequency);

  init_noise_table(noise_table15, 32767, 14);
  init_noise_table(noise_table7, 127, 6);

  reset_sound();
}


bool sound_check_savestate(const u8 *src)
{
  static const char *gvars[] = {
    "on", "buf-base", "gbc-buf-idx", "gbc-last-cpu-ticks",
    "gbc-partial-ticks", "gbc-ms-vol-left", "gbc-ms-vol-right", "gbc-ms-vol"
  };
  static const char *dsvars[] = {
    "status", "volume", "fifo-base", "fifo-top", "fifo-frac", "buf-idx"
  };
  static const char *gsvars[] = {
    "status", "rate", "freq-step", "sample-idx", "tick-cnt", "volume",
    "active", "enable", "env-vol0", "env-vol", "env-dir", "env-status",
    "env-ticks0", "env-ticks", "sweep-status", "sweep-dir", "sweep-ticks0",
    "sweep-ticks","sweep-shift", "wav-type", "wav-bank", "wav-vol",
    "len-status", "len-ticks", "noise-type", "sample-tbl"
  };

  int i;
  const u8 *snddoc = bson_find_key(src, "sound");
  if (!snddoc)
    return false;

  for (i = 0; i < sizeof(gvars)/sizeof(gvars[0]); i++)
    if (!bson_contains_key(snddoc, gvars[i], BSON_TYPE_INT32))
      return false;
  if (!bson_contains_key(snddoc, "wav-samples", BSON_TYPE_BIN))
    return false;


  for (i = 0; i < 2; i++)
  {
    char tn[4] = {'d', 's', '0' + i, 0};
    const u8 *sndchan = bson_find_key(snddoc, tn);
    if (!sndchan)
      return false;

    for (i = 0; i < sizeof(dsvars)/sizeof(dsvars[0]); i++)
      if (!bson_contains_key(sndchan, dsvars[i], BSON_TYPE_INT32))
        return false;

    if (!bson_contains_key(sndchan, "fifo-bytes", BSON_TYPE_BIN))
      return false;
  }

  for (i = 0; i < 4; i++)
  {
    char tn[4] = {'g', 's', '0' + i, 0};
    const u8 *sndchan = bson_find_key(snddoc, tn);
    if (!sndchan)
      return false;

    for (i = 0; i < sizeof(gsvars)/sizeof(gsvars[0]); i++)
      if (!bson_contains_key(sndchan, gsvars[i], BSON_TYPE_INT32))
        return false;
  }

  return true;
}


bool sound_read_savestate(const u8 *src)
{
  int i;
  const u8 *snddoc = bson_find_key(src, "sound");

  if (!(
    bson_read_int32(snddoc, "on", &sound_on) &&
    bson_read_int32(snddoc, "buf-base", &sound_buffer_base) &&
    bson_read_int32(snddoc, "gbc-buf-idx", &gbc_sound_buffer_index) &&
    bson_read_int32(snddoc, "gbc-last-cpu-ticks", &gbc_sound_last_cpu_ticks) &&
    bson_read_int32(snddoc, "gbc-partial-ticks", &gbc_sound_partial_ticks) &&
    bson_read_int32(snddoc, "gbc-ms-vol-left", &gbc_sound_master_volume_left) &&
    bson_read_int32(snddoc, "gbc-ms-vol-right", &gbc_sound_master_volume_right) &&
    bson_read_int32(snddoc, "gbc-ms-vol", &gbc_sound_master_volume) &&
    bson_read_bytes(snddoc, "wav-samples", wave_samples, sizeof(wave_samples))))
    return false;

  for (i = 0; i < 2; i++)
  {
    direct_sound_struct *ds = &direct_sound_channel[i];
    char tn[4] = {'d', 's', '0' + i, 0};
    const u8 *sndchan = bson_find_key(snddoc, tn);
    if (!(
      bson_read_int32(sndchan, "status", &ds->status) &&
      bson_read_int32(sndchan, "volume", &ds->volume_halve) &&
      bson_read_int32(sndchan, "fifo-base", &ds->fifo_base) &&
      bson_read_int32(sndchan, "fifo-top", &ds->fifo_top) &&
      bson_read_int32(sndchan, "fifo-frac", &ds->fifo_fractional) &&
      bson_read_bytes(sndchan, "fifo-bytes", ds->fifo, sizeof(ds->fifo)) &&
      bson_read_int32(sndchan, "buf-idx", &ds->buffer_index)))
      return false;
  }

  for (i = 0; i < 4; i++)
  {
    gbc_sound_struct *gs = &gbc_sound_channel[i];
    char tn[4] = {'g', 's', '0' + i, 0};
    const u8 *sndchan = bson_find_key(snddoc, tn);
    if (!(
      bson_read_int32(sndchan, "status", &gs->status) &&
      bson_read_int32(sndchan, "rate", &gs->rate) &&
      bson_read_int32(sndchan, "freq-step", &gs->frequency_step) &&
      bson_read_int32(sndchan, "sample-idx", &gs->sample_index) &&
      bson_read_int32(sndchan, "tick-cnt", &gs->tick_counter) &&
      bson_read_int32(sndchan, "volume", &gs->total_volume) &&
      bson_read_int32(sndchan, "active", &gs->active_flag) &&
      bson_read_int32(sndchan, "enable", &gs->master_enable) &&

      bson_read_int32(sndchan, "env-vol0", &gs->envelope_initial_volume) &&
      bson_read_int32(sndchan, "env-vol", &gs->envelope_volume) &&
      bson_read_int32(sndchan, "env-dir", &gs->envelope_direction) &&
      bson_read_int32(sndchan, "env-status", &gs->envelope_status) &&
      bson_read_int32(sndchan, "env-ticks0", &gs->envelope_initial_ticks) &&
      bson_read_int32(sndchan, "env-ticks", &gs->envelope_ticks) &&
      bson_read_int32(sndchan, "sweep-status", &gs->sweep_status) &&
      bson_read_int32(sndchan, "sweep-dir", &gs->sweep_direction) &&
      bson_read_int32(sndchan, "sweep-ticks0", &gs->sweep_initial_ticks) &&
      bson_read_int32(sndchan, "sweep-ticks", &gs->sweep_ticks) &&
      bson_read_int32(sndchan, "sweep-shift", &gs->sweep_shift) &&
      bson_read_int32(sndchan, "wav-type", &gs->wave_type) &&
      bson_read_int32(sndchan, "wav-bank", &gs->wave_bank) &&
      bson_read_int32(sndchan, "wav-vol", &gs->wave_volume) &&

      bson_read_int32(sndchan, "len-status", &gs->length_status) &&
      bson_read_int32(sndchan, "len-ticks", &gs->length_ticks) &&
      bson_read_int32(sndchan, "noise-type", &gs->noise_type) &&
      bson_read_int32(sndchan, "sample-tbl", &gs->sample_table_idx)))
      return false;
  }

  return true;
}

unsigned sound_write_savestate(u8 *dst)
{
  int i;
  u8 *wbptr, *startp = dst;
  bson_start_document(dst, "sound", wbptr);
  bson_write_int32(dst, "on", sound_on);
  bson_write_int32(dst, "buf-base", sound_buffer_base);
  bson_write_int32(dst, "gbc-buf-idx", gbc_sound_buffer_index);
  bson_write_int32(dst, "gbc-last-cpu-ticks", gbc_sound_last_cpu_ticks);
  bson_write_int32(dst, "gbc-partial-ticks", gbc_sound_partial_ticks);
  bson_write_int32(dst, "gbc-ms-vol-left", gbc_sound_master_volume_left);
  bson_write_int32(dst, "gbc-ms-vol-right", gbc_sound_master_volume_right);
  bson_write_int32(dst, "gbc-ms-vol", gbc_sound_master_volume);
  bson_write_bytes(dst, "wav-samples", wave_samples, sizeof(wave_samples));

  for (i = 0; i < 2; i++)
  {
    u8 *wbptr2;
    char tn[4] = {'d', 's', '0' + i, 0};
    bson_start_document(dst, tn, wbptr2);
    bson_write_int32(dst, "status", direct_sound_channel[i].status);
    bson_write_int32(dst, "volume", direct_sound_channel[i].volume_halve);
    bson_write_int32(dst, "fifo-base", direct_sound_channel[i].fifo_base);
    bson_write_int32(dst, "fifo-top", direct_sound_channel[i].fifo_top);
    bson_write_int32(dst, "fifo-frac", direct_sound_channel[i].fifo_fractional);
    bson_write_bytes(dst, "fifo-bytes", direct_sound_channel[i].fifo,
                          sizeof(direct_sound_channel[i].fifo));
    bson_write_int32(dst, "buf-idx", direct_sound_channel[i].buffer_index);
    bson_finish_document(dst, wbptr2);
  }

  for (i = 0; i < 4; i++)
  {
    gbc_sound_struct *gs = &gbc_sound_channel[i];
    u8 *wbptr2;
    char tn[4] = {'g', 's', '0' + i, 0};
    bson_start_document(dst, tn, wbptr2);
    bson_write_int32(dst, "status", gs->status);
    bson_write_int32(dst, "rate", gs->rate);
    bson_write_int32(dst, "freq-step", gs->frequency_step);
    bson_write_int32(dst, "sample-idx", gs->sample_index);
    bson_write_int32(dst, "tick-cnt", gs->tick_counter);
    bson_write_int32(dst, "volume", gs->total_volume);
    bson_write_int32(dst, "active", gs->active_flag);
    bson_write_int32(dst, "enable", gs->master_enable);

    bson_write_int32(dst, "env-vol0", gs->envelope_initial_volume);
    bson_write_int32(dst, "env-vol", gs->envelope_volume);
    bson_write_int32(dst, "env-dir", gs->envelope_direction);
    bson_write_int32(dst, "env-status", gs->envelope_status);
    bson_write_int32(dst, "env-ticks0", gs->envelope_initial_ticks);
    bson_write_int32(dst, "env-ticks", gs->envelope_ticks);
    bson_write_int32(dst, "sweep-status", gs->sweep_status);
    bson_write_int32(dst, "sweep-dir", gs->sweep_direction);
    bson_write_int32(dst, "sweep-ticks0", gs->sweep_initial_ticks);
    bson_write_int32(dst, "sweep-ticks", gs->sweep_ticks);
    bson_write_int32(dst, "sweep-shift", gs->sweep_shift);
    bson_write_int32(dst, "wav-type", gs->wave_type);
    bson_write_int32(dst, "wav-bank", gs->wave_bank);
    bson_write_int32(dst, "wav-vol", gs->wave_volume);

    bson_write_int32(dst, "len-status", gs->length_status);
    bson_write_int32(dst, "len-ticks", gs->length_ticks);
    bson_write_int32(dst, "noise-type", gs->noise_type);
    bson_write_int32(dst, "sample-tbl", gs->sample_table_idx);

    // No longer used fields, keep for backwards compatibility.
    bson_write_int32(dst, "env-step", 0);
    bson_finish_document(dst, wbptr2);
  }

  bson_finish_document(dst, wbptr);
  return (unsigned int)(dst - startp);
}

static inline s16 sound_soft_gate(s32 sample)
{
   return (s16)sample;
}

u32 sound_read_samples(s16 *out, u32 frames)
{
   u32 i;
   u32 samples_to_read   = frames << 1;
   u32 samples_requested  = samples_to_read;
   /* Get total number of samples in the buffer */
   u32 samples_available = (gbc_sound_buffer_index - sound_buffer_base) & BUFFER_SIZE_MASK;
   /* The last 512 samples are 'in use', and cannot
    * be read out yet */
   samples_available     = (samples_available > 512) ? (samples_available - 512) : 0;
   /* Available sample count must be an even number */
   samples_available     = (samples_available >> 1) << 1;

   sound_read_calls++;
   sound_samples_requested += samples_requested;
   sound_last_samples_available = samples_available;
   if(samples_available > sound_max_samples_available)
      sound_max_samples_available = samples_available;

#ifdef RETRO_GO
   /*
    * On slower MCU targets the audio submit path can occasionally fall behind.
    * Do not let stale GBA samples build up and play late after the screen has
    * moved on; keep only the current submit plus a small safety margin.
    */
   if(samples_available > samples_to_read + 1024)
   {
      u32 excess_samples = samples_available - (samples_to_read + 1024);
      u32 drop_samples = excess_samples >> 3;

      if(drop_samples < 2)
         drop_samples = 2;
      if(drop_samples > 64)
         drop_samples = 64;
      drop_samples &= ~1;

      for(i = 0; i < drop_samples; i++)
         sound_buffer[(sound_buffer_base + i) & BUFFER_SIZE_MASK] = 0;

      sound_buffer_base = (sound_buffer_base + drop_samples) & BUFFER_SIZE_MASK;
      samples_available -= drop_samples;
      sound_stale_samples_dropped += drop_samples;
      sound_drop_events++;
      sound_last_drop_samples = drop_samples;
      if(drop_samples > sound_max_drop_samples)
         sound_max_drop_samples = drop_samples;
   }
#endif

   if (samples_to_read > samples_available)
   {
      u32 underrun_samples = samples_to_read - samples_available;
      sound_underrun_samples += underrun_samples;
      sound_underrun_events++;
      sound_last_underrun_samples = underrun_samples;
      samples_to_read = samples_available;
   }

   for(i = 0; i < samples_to_read; i++)
   {
      u32 source_index   = (sound_buffer_base + i) & BUFFER_SIZE_MASK;
      s32 current_sample = sound_buffer[source_index];
      s32 sample;
      u32 abs_sample;

      sound_buffer[source_index] = 0;

      if(current_sample > 2047)
      {
         current_sample = 2047;
         sound_clip_samples++;
      }
      if(current_sample < -2048)
      {
         current_sample = -2048;
         sound_clip_samples++;
      }

      sample = current_sample * 16;
      abs_sample = (sample < 0) ? (u32)(-sample) : (u32)sample;
      if(abs_sample != 0)
      {
         sound_nonzero_samples++;
         if(abs_sample <= 512)
            sound_low_level_samples++;
         if(abs_sample > sound_peak_sample)
            sound_peak_sample = abs_sample;
      }

      s16 output_sample = sound_soft_gate(sample);
#ifdef RETRO_GO
      u32 channel = i & 1;
      if(sound_have_last_output_sample[channel])
      {
         s32 delta = (s32)output_sample - sound_last_output_sample[channel];
         u32 abs_delta = (delta < 0) ? (u32)(-delta) : (u32)delta;

         if(abs_delta >= SOUND_JUMP_THRESHOLD)
         {
            sound_jump_samples++;
            sound_last_jump = abs_delta;
            if(abs_delta > sound_max_jump)
               sound_max_jump = abs_delta;
            if(i < 2)
               sound_boundary_jumps++;
         }
      }
      sound_last_output_sample[channel] = output_sample;
      sound_have_last_output_sample[channel] = 1;
#endif
      out[i] = output_sample;
   }

   sound_buffer_base += samples_to_read;
   sound_buffer_base &= BUFFER_SIZE_MASK;
   sound_samples_returned += samples_to_read;

   /* Function returns number of frames read */
   return (samples_to_read >> 1);
}

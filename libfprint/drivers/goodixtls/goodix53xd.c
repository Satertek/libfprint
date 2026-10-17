// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (C) 2021 Michael Teuscher <michael.teuscher@pm.me>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "fp-device.h"
#include "fp-image-device.h"
#include "fp-image.h"
#include "fpi-assembling.h"
#include "fpi-context.h"
#include "fpi-image-device.h"
#include "fpi-image.h"
#include "fpi-ssm.h"
#include "glibconfig.h"
#include "gusb/gusb-device.h"
#include <stdio.h>
#include <stdlib.h>
#define FP_COMPONENT "goodixtls53xd"

#include <glib.h>
#include <string.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodix53xd.h"

#include <math.h>

#define GOODIX53XD_WIDTH 64
#define GOODIX53XD_HEIGHT 80
#define GOODIX53XD_SCAN_WIDTH 64
#define GOODIX53XD_FRAME_SIZE (GOODIX53XD_WIDTH * GOODIX53XD_HEIGHT)
// For every 4 pixels there are 6 bytes and there are 8 extra start bytes and 5
// extra end
#define GOODIX53XD_RAW_FRAME_SIZE                                               \
    (GOODIX53XD_HEIGHT * GOODIX53XD_SCAN_WIDTH) / 4 * 6
#define GOODIX53XD_CAP_FRAMES 10 // Number of frames we capture per swipe

typedef unsigned short Goodix53xdPix;

struct _FpiDeviceGoodixTls53XD {
  FpiDeviceGoodixTls parent;

  guint8* otp;

  GSList* frames;

  Goodix53xdPix empty_img[GOODIX53XD_FRAME_SIZE];
};

G_DECLARE_FINAL_TYPE(FpiDeviceGoodixTls53XD, fpi_device_goodixtls53xd, FPI,
                     DEVICE_GOODIXTLS53XD, FpiDeviceGoodixTls);

G_DEFINE_TYPE(FpiDeviceGoodixTls53XD, fpi_device_goodixtls53xd,
              FPI_TYPE_DEVICE_GOODIXTLS);

// ---- ACTIVE SECTION START ----

enum activate_states {
    ACTIVATE_READ_AND_NOP,
    ACTIVATE_ENABLE_CHIP,
    ACTIVATE_NOP,
    ACTIVATE_CHECK_FW_VER,
    ACTIVATE_CHECK_PSK,
    ACTIVATE_RESET,
    ACTIVATE_OTP,
    ACTIVATE_SET_MCU_IDLE,
    ACTIVATE_SET_MCU_CONFIG,
    ACTIVATE_NUM_STATES,
};

static void check_none(FpDevice *dev, gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}

static void check_firmware_version(FpDevice *dev, gchar *firmware,
                                   gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fp_dbg("Device firmware: \"%s\"", firmware);

  if (strcmp(firmware, GOODIX_53XD_FIRMWARE_VERSION)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device firmware: \"%s\"", firmware);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}

static void check_reset(FpDevice *dev, gboolean success, guint16 number,
                        gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (!success) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to reset device");
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fp_dbg("Device reset number: %d", number);

  if (number != GOODIX_53XD_RESET_NUMBER) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device reset number: %d", number);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}

static void check_preset_psk_read(FpDevice *dev, gboolean success,
                                  guint32 flags, guint8 *psk, guint16 length,
                                  gpointer user_data, GError *error) {
  g_autofree gchar *psk_str = data_to_str(psk, length);

  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (!success) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to read PSK from device");
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fp_dbg("Device PSK: 0x%s", psk_str);
  fp_dbg("Device PSK flags: 0x%08x", flags);

  if (flags != GOODIX_53XD_PSK_FLAGS) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PSK flags: 0x%08x", flags);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (length != sizeof(goodix_53xd_psk_0)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PSK: 0x%s", psk_str);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (memcmp(psk, goodix_53xd_psk_0, sizeof(goodix_53xd_psk_0))) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PSK: 0x%s", psk_str);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}
static void check_idle(FpDevice* dev, gpointer user_data, GError* err)
{

    if (err) {
        fpi_ssm_mark_failed(user_data, err);
        return;
    }
    fpi_ssm_next_state(user_data);
}
static void check_config_upload(FpDevice* dev, gboolean success,
                                gpointer user_data, GError* error)
{
    if (error) {
        fpi_ssm_mark_failed(user_data, error);
    }
    else if (!success) {
        fpi_ssm_mark_failed(user_data,
                            g_error_new(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                        "failed to upload mcu config"));
    }
    else {
        fpi_ssm_next_state(user_data);
    }
}
static void check_powerdown_scan_freq(FpDevice* dev, gboolean success,
                                      gpointer user_data, GError* error)
{
    if (error) {
        fpi_ssm_mark_failed(user_data, error);
    }
    else if (!success) {
        fpi_ssm_mark_failed(user_data,
                            g_error_new(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                        "failed to set powerdown freq"));
    }
    fpi_ssm_next_state(user_data);
}

enum otp_write_states {
    OTP_WRITE_1,
    OTP_WRITE_2,

    OTP_WRITE_NUM,
};

static void otp_write_run(FpiSsm* ssm, FpDevice* dev)
{
    /*FpiDeviceGoodixTls53XD* self = FPI_DEVICE_GOODIXTLS53XD(dev);
    switch (fpi_ssm_get_cur_state(ssm)) {
    case OTP_WRITE_1:
        guint8 sensor1[] = {0x0a, 0x02};
        goodix_send_write_sensor_register(
        dev, 0x022c, sensor1, check_none,
        ssm);
        break;
    case OTP_WRITE_2:
        guint8 sensor2[] = {0x0a, 0x03};
        goodix_send_write_sensor_register(
            dev, 0x022c, sensor2, check_none,
            ssm);
        fpi_ssm_next_state(ssm);
        break;
    }*/
}

static void read_otp_callback(FpDevice* dev, guint8* data, guint16 len,
                              gpointer ssm, GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }
    if (len < 64) {
        fpi_ssm_mark_failed(ssm, g_error_new(FP_DEVICE_ERROR,
                                             FP_DEVICE_ERROR_DATA_INVALID,
                                             "OTP is invalid (len: %d)", 64));
        return;
    }
    FpiDeviceGoodixTls53XD* self = FPI_DEVICE_GOODIXTLS53XD(dev);
    self->otp = malloc(64);
    memcpy(self->otp, data, len);
    fpi_ssm_next_state(ssm);
}

static void activate_run_state(FpiSsm* ssm, FpDevice* dev)
{

    switch (fpi_ssm_get_cur_state(ssm)) {
    case ACTIVATE_READ_AND_NOP:
        // Nop seems to clear the previous command buffer. But we are
        // unable to do so.
        goodix_start_read_loop(dev);
        goodix_send_nop(dev, check_none, ssm);
        break;

    case ACTIVATE_ENABLE_CHIP:
      goodix_send_enable_chip(dev, TRUE, check_none, ssm);
      break;

    case ACTIVATE_NOP:
      goodix_send_nop(dev, check_none, ssm);
      break;

    case ACTIVATE_CHECK_FW_VER:
      goodix_send_firmware_version(dev, check_firmware_version, ssm);
      break;

    case ACTIVATE_CHECK_PSK:
      goodix_send_preset_psk_read(dev, GOODIX_53XD_PSK_FLAGS, 32,
                                  check_preset_psk_read, ssm);
      break;

    case ACTIVATE_RESET:
      goodix_send_reset(dev, TRUE, 20, check_reset, ssm);
      break;

    case ACTIVATE_OTP:
      goodix_send_read_otp(dev, read_otp_callback, ssm);
      break;

    case ACTIVATE_SET_MCU_IDLE:
        goodix_send_mcu_switch_to_idle_mode(dev, 20, check_idle, ssm);
        break;

    case ACTIVATE_SET_MCU_CONFIG:
        goodix_send_upload_config_mcu(dev, goodix_53xd_config,
                                      sizeof(goodix_53xd_config), NULL,
                                      check_config_upload, ssm);
        break;
    }
}

static void tls_activation_complete(FpDevice* dev, gpointer user_data,
                                    GError* error)
{
    if (error) {
        fp_err("failed to complete tls activation: %s", error->message);
        return;
    }
    FpImageDevice* image_dev = FP_IMAGE_DEVICE(dev);

    fpi_image_device_activate_complete(image_dev, error);
}

static void activate_complete(FpiSsm* ssm, FpDevice* dev, GError* error)
{
    G_DEBUG_HERE();
    if (!error)
        goodix_tls(dev, tls_activation_complete, NULL);
    else {
        fp_err("failed during activation: %s (code: %d)", error->message,
               error->code);
        fpi_image_device_activate_complete(FP_IMAGE_DEVICE(dev), error);
    }
}

// ---- ACTIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- SCAN SECTION START ----

enum SCAN_STAGES {
    SCAN_STAGE_PRE_FDT_MODE_1,
    SCAN_STAGE_PRE_FDT_MODE_2,
    SCAN_STAGE_SLEEP_MODE,
    SCAN_STAGE_QUERY_MCU_STATE,
    SCAN_STAGE_FDT_DOWN_1,
    SCAN_STAGE_FDT_DOWN_WAIT,
    SCAN_STAGE_POST_FDT_MODE_1,
    SCAN_STAGE_POST_FDT_MODE_2,
    SCAN_STAGE_WRITE_SENSOR,
    SCAN_STAGE_GET_IMG,

    SCAN_STAGE_NUM,
};

static void check_none_cmd(FpDevice* dev, guint8* data, guint16 len,
                           gpointer ssm, GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }
    fpi_ssm_next_state(ssm);
}

static void check_none_cmd_none(FpDevice* dev, gpointer ssm, GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }
    fpi_ssm_next_state(ssm);
}

static void decode_frame(Goodix53xdPix frame[GOODIX53XD_FRAME_SIZE],
                         const guint8* raw_frame)
{
    Goodix53xdPix uncropped[GOODIX53XD_SCAN_WIDTH * GOODIX53XD_HEIGHT];
    Goodix53xdPix* pix = uncropped;
    for (int i = 0; i < GOODIX53XD_RAW_FRAME_SIZE; i += 6) {
        const guint8* chunk = raw_frame + i;
        *pix++ = ((chunk[0] & 0xf) << 8) + chunk[1];
        *pix++ = (chunk[3] << 4) + (chunk[0] >> 4);
        *pix++ = ((chunk[5] & 0xf) << 8) + chunk[2];
        *pix++ = (chunk[4] << 4) + (chunk[5] >> 4);
    }

    for (int y = 0; y != GOODIX53XD_HEIGHT; ++y) {
        for (int x = 0; x != GOODIX53XD_WIDTH; ++x) {
            const int idx = x + y * GOODIX53XD_SCAN_WIDTH;
            frame[x + y * GOODIX53XD_WIDTH] = uncropped[idx];
        }
    }
}

/**
 * @brief Squashes the 12 bit pixels of a raw frame into 8 bit pixels used by libfprint.
 */
static void squash_frame_linear(Goodix53xdPix* frame, guint8* squashed)
{
    Goodix53xdPix min = 0xffff;
    Goodix53xdPix max = 0;

    for (int y = 1; y < GOODIX53XD_HEIGHT - 1; ++y) {
        for (int x = 1; x < GOODIX53XD_WIDTH - 1; ++x) {
            const Goodix53xdPix pix = frame[x + y * GOODIX53XD_WIDTH];
            if (pix < min) min = pix;
            if (pix > max) max = pix;
        }
    }

    if (max <= min) {
        memset(squashed, 0xff, GOODIX53XD_FRAME_SIZE);
        return;
    }

    for (int y = 0; y < GOODIX53XD_HEIGHT; ++y) {
        for (int x = 0; x < GOODIX53XD_WIDTH; ++x) {
            int idx = x + y * GOODIX53XD_WIDTH;
            if (x == 0 || x == GOODIX53XD_WIDTH - 1 || y == 0 || y == GOODIX53XD_HEIGHT - 1) {
                squashed[idx] = 0xff;
            } else {
                Goodix53xdPix pix = frame[idx];
                if (pix < min) pix = min;
                if (pix > max) pix = max;
                squashed[idx] = (guint8)((pix - min) * 0xff / (max - min));
            }
        }
    }
}

static void scan_on_read_img(FpDevice* dev, guint8* data, guint16 len,
                             gpointer ssm, GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }

    FpImageDevice* img_dev = FP_IMAGE_DEVICE(dev);
    Goodix53xdPix raw_frame[GOODIX53XD_FRAME_SIZE];
    decode_frame(raw_frame, data);

    Goodix53xdPix rmin = 0xffff, rmax = 0;
    for (int i = 0; i < GOODIX53XD_FRAME_SIZE; i++) {
        if (raw_frame[i] < rmin) rmin = raw_frame[i];
        if (raw_frame[i] > rmax) rmax = raw_frame[i];
    }
    fp_dbg("Decoded frame: raw min=%u, max=%u", rmin, rmax);

    FpImage* img = fp_image_new(GOODIX53XD_WIDTH, GOODIX53XD_HEIGHT);
    squash_frame_linear(raw_frame, img->data);

    fpi_image_device_image_captured(img_dev, img);
    fpi_image_device_report_finger_status(img_dev, FALSE);

    fpi_ssm_next_state(ssm);
}

static void scan_get_img(FpDevice* dev, FpiSsm* ssm)
{
    guint8 payload[] = {0x41, 0x03, 0xa5, 0x00, 0x9f, 0x00, 0xa5, 0x00, 0xa1, 0x00};
    goodix_tls_read_image(dev, payload, sizeof(payload), scan_on_read_img, ssm);
}

static void write_sensor_complete(FpDevice *dev, gpointer user_data, GError *error) 
{
    if (error) {
        fp_err("failed to write sensor register: %s (code: %d)", error->message, error->code);
        fpi_ssm_mark_failed(user_data, error);
        return;
    }
    fpi_ssm_next_state(user_data);
}

static guint8 fdt_mode_zeroes_53xd[] = {
    0x0d, 0x01, 0x28, 0x01, 0x22, 0x01, 0x28, 0x01,
    0x24, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00
};

static guint8 fdt_down_thresholds_53xd[] = {
    0x8c, 0x01, 0x28, 0x01, 0x22, 0x01, 0x28, 0x01,
    0x24, 0x01, 0x91, 0x91, 0x8b, 0x8b, 0x96, 0x96,
    0x91, 0x91, 0x98, 0x98, 0x90, 0x90, 0x92, 0x92,
    0x88, 0x88, 0x00
};

static guint8 fdt_mode_thresholds_53xd[] = {
    0x0d, 0x01, 0x28, 0x01, 0x22, 0x01, 0x28, 0x01,
    0x24, 0x01, 0x91, 0x91, 0x8b, 0x8b, 0x96, 0x96,
    0x91, 0x91, 0x98, 0x98, 0x90, 0x90, 0x92, 0x92,
    0x88, 0x88, 0x00
};

static void update_fdt_thresholds(const guint8 *data, guint16 len)
{
    if (len < 20)
        return;

    const guint8 *fdt_data = data + 4;
    for (int i = 0; i < 8; i++) {
        guint16 fdt_val = fdt_data[i * 2] | (fdt_data[i * 2 + 1] << 8);
        guint8 base_byte = (guint8)(fdt_val >> 1);
        fdt_down_thresholds_53xd[10 + i * 2] = base_byte;
        fdt_down_thresholds_53xd[10 + i * 2 + 1] = base_byte;
        fdt_mode_thresholds_53xd[10 + i * 2] = base_byte;
        fdt_mode_thresholds_53xd[10 + i * 2 + 1] = base_byte;
    }
    fp_dbg("Updated FDT thresholds from sensor baseline: 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x",
           fdt_down_thresholds_53xd[10], fdt_down_thresholds_53xd[12],
           fdt_down_thresholds_53xd[14], fdt_down_thresholds_53xd[16],
           fdt_down_thresholds_53xd[18], fdt_down_thresholds_53xd[20],
           fdt_down_thresholds_53xd[22], fdt_down_thresholds_53xd[24]);
}

static void on_pre_fdt_mode_2(FpDevice* dev, guint8* data, guint16 len,
                              gpointer ssm, GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }
    if (data && len >= 20) {
        update_fdt_thresholds(data, len);
    }
    fpi_ssm_next_state(ssm);
}

static void on_finger_touched(FpDevice* dev, guint8* data, guint16 len,
                              gpointer ssm, GError* err)
{
    if (err) {
        fpi_ssm_mark_failed(ssm, err);
        return;
    }
    gchar *str = data_to_str(data, len);
    fp_dbg("0x32 reply len=%d data=%s", len, str);
    g_free(str);

    guint16 touch_flag = 0;
    if (len >= 4) {
        touch_flag = data[2] | (data[3] << 8);
    }
    fp_dbg("FDT touch_flag: 0x%04x", touch_flag);

    if (touch_flag == 0) {
        fp_dbg("Baseline/temperature drift event (touch_flag=0), updating baseline and re-arming...");
        if (len >= 20) {
            update_fdt_thresholds(data, len);
        }
        fpi_ssm_jump_to_state(ssm, SCAN_STAGE_FDT_DOWN_1);
        return;
    }

    fp_dbg("Physical finger touch detected on sensor (touch_flag=0x%04x)!", touch_flag);
    fpi_image_device_report_finger_status(FP_IMAGE_DEVICE(dev), TRUE);
    fpi_ssm_next_state(ssm);
}

static const guint8 query_mcu_state_53xd[] = { 0x01, 0x01, 0x01 };

static void scan_run_state(FpiSsm* ssm, FpDevice* dev)
{
    switch (fpi_ssm_get_cur_state(ssm)) {
    case SCAN_STAGE_PRE_FDT_MODE_1:
        fdt_mode_zeroes_53xd[26] = 0x00;
        goodix_send_mcu_switch_to_fdt_mode(dev, fdt_mode_zeroes_53xd,
                                           sizeof(fdt_mode_zeroes_53xd), FALSE, NULL,
                                           check_none_cmd, ssm);
        break;

    case SCAN_STAGE_PRE_FDT_MODE_2:
        fdt_mode_zeroes_53xd[26] = 0x01;
        goodix_send_mcu_switch_to_fdt_mode(dev, fdt_mode_zeroes_53xd,
                                           sizeof(fdt_mode_zeroes_53xd), TRUE, NULL,
                                           on_pre_fdt_mode_2, ssm);
        break;

    case SCAN_STAGE_SLEEP_MODE:
        goodix_send_mcu_switch_to_sleep_mode(dev, check_none_cmd_none, ssm);
        break;

    case SCAN_STAGE_QUERY_MCU_STATE:
        goodix_send_query_mcu_state_raw(dev, query_mcu_state_53xd,
                                        sizeof(query_mcu_state_53xd), FALSE,
                                        check_none_cmd, ssm);
        break;

    case SCAN_STAGE_FDT_DOWN_1:
        fdt_down_thresholds_53xd[26] = 0x00;
        goodix_send_mcu_switch_to_fdt_down(dev, fdt_down_thresholds_53xd,
                                           sizeof(fdt_down_thresholds_53xd), FALSE, NULL,
                                           check_none_cmd, ssm);
        break;

    case SCAN_STAGE_FDT_DOWN_WAIT:
        fp_dbg("Waiting for finger touch...");
        fdt_down_thresholds_53xd[26] = 0x01;
        goodix_send_mcu_switch_to_fdt_down(dev, fdt_down_thresholds_53xd,
                                           sizeof(fdt_down_thresholds_53xd), TRUE, NULL,
                                           on_finger_touched, ssm);
        break;

    case SCAN_STAGE_POST_FDT_MODE_1:
        fdt_mode_thresholds_53xd[26] = 0x00;
        goodix_send_mcu_switch_to_fdt_mode(dev, fdt_mode_thresholds_53xd,
                                           sizeof(fdt_mode_thresholds_53xd), FALSE, NULL,
                                           check_none_cmd, ssm);
        break;

    case SCAN_STAGE_POST_FDT_MODE_2:
        fdt_mode_thresholds_53xd[26] = 0x01;
        goodix_send_mcu_switch_to_fdt_mode(dev, fdt_mode_thresholds_53xd,
                                           sizeof(fdt_mode_thresholds_53xd), TRUE, NULL,
                                           check_none_cmd, ssm);
        break;

    case SCAN_STAGE_WRITE_SENSOR: {
        guint16 payload = 0x0305;
        goodix_send_write_sensor_register(dev, 0x022c, payload, write_sensor_complete, ssm);
        break;
    }

    case SCAN_STAGE_GET_IMG:
        scan_get_img(dev, ssm);
        break;
    }
}

static void scan_complete(FpiSsm* ssm, FpDevice* dev, GError* error)
{
    if (error) {
        fp_err("failed to scan: %s (code: %d)", error->message, error->code);
        fpi_image_device_session_error(FP_IMAGE_DEVICE(dev), error);
        return;
    }
    fp_dbg("finished scan");
}

static void scan_start(FpiDeviceGoodixTls53XD* dev)
{
    fpi_ssm_start(fpi_ssm_new(FP_DEVICE(dev), scan_run_state, SCAN_STAGE_NUM),
                  scan_complete);
}

// ---- SCAN SECTION END ----

// ---- DEV SECTION START ----

static void dev_init(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);
  GError *error = NULL;

  if (goodix_dev_init(dev, &error)) {
    fpi_image_device_open_complete(img_dev, error);
    return;
  }

  fpi_image_device_open_complete(img_dev, NULL);
}

static void dev_deinit(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);
  GError *error = NULL;

  if (goodix_dev_deinit(dev, &error)) {
    fpi_image_device_close_complete(img_dev, error);
    return;
  }

  fpi_image_device_close_complete(img_dev, NULL);
}

static void dev_activate(FpImageDevice *img_dev) {
    FpDevice* dev = FP_DEVICE(img_dev);

    fpi_ssm_start(fpi_ssm_new(dev, activate_run_state, ACTIVATE_NUM_STATES),
                  activate_complete);
}



static void dev_change_state(FpImageDevice* img_dev, FpiImageDeviceState state)
{
    FpiDeviceGoodixTls53XD* self = FPI_DEVICE_GOODIXTLS53XD(img_dev);
    G_DEBUG_HERE();

    if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON) {
        scan_start(self);
    }
}

static void goodix53xd_reset_state(FpiDeviceGoodixTls53XD* self) {}

static void dev_deactivate(FpImageDevice *img_dev) {
    FpDevice* dev = FP_DEVICE(img_dev);
    goodix_reset_state(dev);
    GError* error = NULL;
    goodix_shutdown_tls(dev, &error);
    goodix53xd_reset_state(FPI_DEVICE_GOODIXTLS53XD(img_dev));
    fpi_image_device_deactivate_complete(img_dev, error);
}

// ---- DEV SECTION END ----

static void fpi_device_goodixtls53xd_init(FpiDeviceGoodixTls53XD* self)
{
    self->frames = NULL;
}

/* The TLS PSK is 32 zero bytes for the 538d (after the sensor has been
 * initialized with goodix-fp-dump). This is distinct from goodix_53xd_psk_0,
 * which is the preset_psk verification hash (sha256 of these zeros) that the
 * driver compares against the device's preset_psk_read reply. */
static const guint8 goodix_53xd_tls_psk[32] = {0};

static const guint8* goodix53xd_get_psk(guint16* len) {
  *len = sizeof(goodix_53xd_tls_psk);
  return goodix_53xd_tls_psk;
}

static void fpi_device_goodixtls53xd_class_init(
    FpiDeviceGoodixTls53XDClass *class) {
  FpiDeviceGoodixTlsClass *gx_class = FPI_DEVICE_GOODIXTLS_CLASS(class);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS(class);
  FpImageDeviceClass *img_dev_class = FP_IMAGE_DEVICE_CLASS(class);

  gx_class->interface = GOODIX_53XD_INTERFACE;
  gx_class->ep_in = GOODIX_53XD_EP_IN;
  gx_class->ep_out = GOODIX_53XD_EP_OUT;
  gx_class->get_psk = goodix53xd_get_psk;

  dev_class->id = "goodixtls53xd";
  dev_class->full_name = "Goodix TLS Fingerprint Sensor 53XD";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;

  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = 10;

  img_dev_class->bz3_threshold = 18;
  img_dev_class->img_width = GOODIX53XD_WIDTH;
  img_dev_class->img_height = GOODIX53XD_HEIGHT;

  img_dev_class->img_open = dev_init;
  img_dev_class->img_close = dev_deinit;
  img_dev_class->activate = dev_activate;
  img_dev_class->change_state = dev_change_state;
  img_dev_class->deactivate = dev_deactivate;

  fpi_device_class_auto_initialize_features(dev_class);
}

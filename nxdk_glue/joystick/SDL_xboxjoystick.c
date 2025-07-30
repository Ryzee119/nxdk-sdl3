// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2019 Lucas Eriksson
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#include "SDL_xboxjoystick.h"
#include <SDL3/SDL_gamepad.h>

#include <assert.h>
#include <usbh_lib.h>

#include <usb/libusbohci/inc/hub.h>
#include <xboxkrnl/xboxkrnl.h>
#include <xid_driver.h>

// #define SDL_JOYSTICK_XBOX_DEBUG
#ifdef SDL_JOYSTICK_XBOX_DEBUG
#include <hal/debug.h>
#define JOY_DBGMSG debugPrint
#else
#define JOY_DBGMSG(...)
#endif

#define MAX_JOYSTICKS   CONFIG_XID_MAX_DEV
#define MAX_PACKET_SIZE 32
#define BUTTON_DEADZONE 0x20

// XINPUT defines and struct format from
// https://docs.microsoft.com/en-us/windows/win32/api/xinput/ns-xinput-xinput_gamepad
#define XINPUT_GAMEPAD_DPAD_UP        0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN      0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT      0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT     0x0008
#define XINPUT_GAMEPAD_START          0x0010
#define XINPUT_GAMEPAD_BACK           0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB     0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB    0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER  0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A              0x1000
#define XINPUT_GAMEPAD_B              0x2000
#define XINPUT_GAMEPAD_X              0x4000
#define XINPUT_GAMEPAD_Y              0x8000
#define MAX_PACKET_SIZE               32

typedef struct _XINPUT_GAMEPAD
{
    Uint16 wButtons;
    Uint8 bLeftTrigger;
    Uint8 bRightTrigger;
    Sint16 sThumbLX;
    Sint16 sThumbLY;
    Sint16 sThumbRX;
    Sint16 sThumbRY;
} XINPUT_GAMEPAD, *PXINPUT_GAMEPAD;

// Struct linked to SDL_Joystick
typedef struct joystick_hwdata
{
    xid_dev_t *xid_dev;
    Uint8 raw_data[MAX_PACKET_SIZE];
    Uint16 current_rumble[2];
} joystick_hwdata, *pjoystick_hwdata;

static Sint32 parse_input_data(xid_dev_t *xid_dev, PXINPUT_GAMEPAD controller, Uint8 *rdata);

static Sint32 SDL_XBOX_JoystickGetDevicePlayerIndex(Sint32 device_index);

// Create SDL events for connection/disconnection. These events can then be handled in the user application
static void connection_callback(xid_dev_t *xid_dev, int status)
{
    (void)status;
    JOY_DBGMSG("connection_callback: uid %i connected \n", xid_dev->uid);
    SDL_PrivateJoystickAdded(xid_dev->uid);
}

static void disconnect_callback(xid_dev_t *xid_dev, int status)
{
    (void)status;
    JOY_DBGMSG("disconnect_callback uid %i disconnected\n", xid_dev->uid);
    SDL_PrivateJoystickRemoved(xid_dev->uid);
}

static void int_read_callback(UTR_T *utr)
{
    xid_dev_t *xid_dev = (xid_dev_t *)utr->context;

    if (utr->status < 0 || xid_dev == NULL || xid_dev->user_data == NULL) {
        return;
    }

    SDL_Joystick *joy = (SDL_Joystick *)xid_dev->user_data;

    // Cap data len to buffer size.
    Uint32 data_len = utr->xfer_len;
    if (data_len > MAX_PACKET_SIZE)
        data_len = MAX_PACKET_SIZE;

    if (joy->hwdata != NULL) {
        SDL_memcpy(joy->hwdata->raw_data, utr->buff, data_len);

        // Re-queue the USB transfer
        utr->xfer_len = 0;
        utr->bIsTransferDone = 0;
        usbh_int_xfer(utr);
    }
}

static xid_dev_t *xid_from_device_index(Sint32 device_index)
{
    xid_dev_t *xid_dev = usbh_xid_get_device_list();

    Sint32 i = 0;
    // Scan the xid_dev linked list and finds the nth xid_dev that is a gamepad.
    while (xid_dev != NULL && i <= device_index) {
        // FIXME: Include xremote and steel battalion in the joystick API.
        if (xid_dev->xid_desc.bType == XID_TYPE_GAMECONTROLLER) {
            if (i == device_index)
                return xid_dev;
            i++;
        }
        xid_dev = xid_dev->next;
    }
    assert(0);
    return NULL;
}

static Sint32 xid_get_device_port(xid_dev_t *xid_dev)
{
    UDEV_T *udev, *parent_udev;

    udev = xid_dev->iface->udev;
    ULONG has_internal_hub = XboxHardwareInfo.Flags & XBOX_HW_FLAG_INTERNAL_USB_HUB;
    while (udev != NULL) {
        parent_udev = NULL;
        if (udev->parent != NULL) {
            parent_udev = udev->parent->iface->udev;
        }

        if ((has_internal_hub && parent_udev->parent == NULL) || (!has_internal_hub && udev->parent == NULL)) {
            switch (udev->port_num) {
            case 3:
                return 1;
            case 4:
                return 2;
            case 1:
                return 3;
            case 2:
                return 4;
            default:
                return 0;
            }
        }
        udev = parent_udev;
    }
    return 0;
}

static bool core_has_init = false;
static bool SDL_XBOX_JoystickInit(void)
{
    if (!core_has_init) {
        usbh_core_init();
        usbh_xid_init();
        core_has_init = true;
    }
    usbh_install_xid_conn_callback(connection_callback, disconnect_callback);

#ifndef SDL_DISABLE_JOYSTICK_INIT_DELAY
    // Ensure all connected devices have completed enumeration and are running
    // This wouldnt be required if user applications correctly handled connection events, but most dont
    // This needs to allow time for port reset, debounce, device reset etc. ~200ms per device. ~500ms is time for 1 hub + 1 controller.
    for (Sint32 i = 0; i < 500; i++) {
        usbh_pooling_hubs();
        SDL_Delay(1);
    }
#endif
    return true;
}

static int SDL_XBOX_JoystickGetCount(void)
{
    int pad_cnt = 0;
    xid_dev_t *xid_dev = usbh_xid_get_device_list();
    while (xid_dev != NULL) {
        // FIXME: Include xremote and steel battalion in the joystick API.
        if (xid_dev->xid_desc.bType == XID_TYPE_GAMECONTROLLER) {
            pad_cnt++;
        }
        xid_dev = xid_dev->next;
    }
    JOY_DBGMSG("SDL_XBOX_JoystickGetCount: Found %i pads\n", pad_cnt);
    return pad_cnt;
}

static void SDL_XBOX_JoystickDetect(void)
{
    usbh_pooling_hubs();
}

static bool SDL_XBOX_JoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    (void)version;
    (void)name;
    xid_dev_t *xid_dev = usbh_xid_get_device_list();
    while (xid_dev != NULL) {
        if (xid_dev->idVendor == vendor_id && xid_dev->idProduct == product_id) {
            return true;
        }
        xid_dev = xid_dev->next;
    }
    return false;
}

static const char *SDL_XBOX_JoystickGetDeviceName(int device_index)
{
    xid_dev_t *xid_dev = xid_from_device_index(device_index);

    if (xid_dev == NULL || device_index >= MAX_JOYSTICKS)
        return "Invalid device index";

    static char name[MAX_JOYSTICKS][64];
    Uint32 max_len = sizeof(name[device_index]);

    Sint32 player_index = SDL_XBOX_JoystickGetDevicePlayerIndex(device_index);
    switch (xid_dev->xid_desc.bType) {
    case XID_TYPE_GAMECONTROLLER:
        SDL_snprintf(name[device_index], max_len, "Original Xbox Controller #%u", player_index);
        break;
    case XID_TYPE_XREMOTE:
        SDL_snprintf(name[device_index], max_len, "Original Xbox IR Remote #%u", player_index);
        break;
    case XID_TYPE_STEELBATTALION:
        SDL_snprintf(name[device_index], max_len, "Steel Battalion Controller #%u", player_index);
        break;
    }

    return name[device_index];
}

static const char *SDL_XBOX_JoystickGetDevicePath(int device_index)
{
    (void)device_index;
    return NULL;
}

static int SDL_XBOX_JoystickGetDeviceSteamVirtualGamepadSlot(int device_index)
{
    (void)device_index;
    return -1;
}

// Returns the port number the device is connected to
// 1 = Port 1, 2 = Port 2, etc.
static Sint32 SDL_XBOX_JoystickGetDevicePlayerIndex(Sint32 device_index)
{
    xid_dev_t *xid_dev = xid_from_device_index(device_index);

    if (xid_dev == NULL) {
        return -1;
    }

    Sint32 player_index = xid_get_device_port(xid_dev);
    if (player_index == 0) { // fallback to device_index if xid_get_device_port fails (returns 0)
        player_index = device_index;
    }
    JOY_DBGMSG("SDL_XBOX_JoystickGetDevicePlayerIndex: %i\n", player_index);

    return player_index;
}

static void SDL_XBOX_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
    (void)device_index;
    (void)player_index;
}

static SDL_GUID SDL_XBOX_JoystickGetDeviceGUID(Sint32 device_index)
{
    xid_dev_t *xid_dev = xid_from_device_index(device_index);

    SDL_GUID ret;
    SDL_zero(ret);

    if (xid_dev != NULL) {
        // Format based on SDL_gamecontrollerdb.h
        ret.data[0] = 0x03;
        ret.data[4] = xid_dev->idVendor & 0xFF;
        ret.data[5] = (xid_dev->idVendor >> 8) & 0xFF;
        ret.data[8] = xid_dev->idProduct & 0xFF;
        ret.data[9] = (xid_dev->idProduct >> 8) & 0xFF;
    }
    return ret;
}

static SDL_JoystickID SDL_XBOX_JoystickGetDeviceInstanceID(Sint32 device_index)
{
    xid_dev_t *xid_dev = xid_from_device_index(device_index);

    SDL_JoystickID ret;
    SDL_zero(ret);

    if (xid_dev != NULL) {
        SDL_memcpy(&ret, &xid_dev->uid, sizeof(xid_dev->uid));
    }
    JOY_DBGMSG("SDL_XBOX_JoystickGetDeviceInstanceID: %i\n", xid_dev->uid);
    return ret;
}

static bool SDL_XBOX_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    xid_dev_t *xid_dev = xid_from_device_index(device_index);

    if (xid_dev == NULL) {
        JOY_DBGMSG("SDL_XBOX_JoystickOpen: Could not find device index %i\n", device_index);
        return false;
    }

    joystick->hwdata = (pjoystick_hwdata)SDL_malloc(sizeof(joystick_hwdata));
    assert(joystick->hwdata != NULL);
    SDL_zerop(joystick->hwdata);

    joystick->hwdata->xid_dev = xid_dev;
    joystick->hwdata->xid_dev->user_data = (void *)joystick;
    joystick->guid = SDL_XBOX_JoystickGetDeviceGUID(device_index);

    switch (xid_dev->xid_desc.bType) {
    case XID_TYPE_GAMECONTROLLER:
        joystick->naxes = 6;     /* LStickY, LStickX, LTrigg, RStickY, RStickX, RTrigg */
        joystick->nballs = 0;    /* No balls here */
        joystick->nhats = 1;     /* D-pad */
        joystick->nbuttons = 10; /* A, B, X, Y, RB, LB, Back, Start, LThumb, RThumb */
        break;
    case XID_TYPE_XREMOTE:
        joystick->naxes = 0;
        joystick->nballs = 0;
        joystick->nhats = 0;
        joystick->nbuttons = 27;
        break;
    case XID_TYPE_STEELBATTALION:
        joystick->naxes = 10; // Tuner dial and gear level are treated like an axis
        joystick->nballs = 0;
        joystick->nhats = 0;
        joystick->nbuttons = 39; // This includes the toggle switches
        break;
    default:
        SDL_free(joystick->hwdata);
        joystick->hwdata = NULL;
        return false;
    }

    JOY_DBGMSG("JoystickOpened:\n");
    JOY_DBGMSG("joystick device_index: %i\n", device_index);
    JOY_DBGMSG("joystick player_index: %i\n", joystick->player_index);
    JOY_DBGMSG("joystick uid: %i\n", xid_dev->uid);
    JOY_DBGMSG("joystick name: %s\n", SDL_XBOX_JoystickGetDeviceName(device_index));

    // Start reading interrupt pipe
    usbh_xid_read(xid_dev, 0, (void *)int_read_callback);

    return true;
}

static bool SDL_XBOX_JoystickRumble(SDL_Joystick *joystick,
                                    Uint16 low_frequency_rumble,
                                    Uint16 high_frequency_rumble)
{

    // Check if rumble values are new values.
    if (joystick->hwdata->current_rumble[0] == low_frequency_rumble &&
        joystick->hwdata->current_rumble[1] == high_frequency_rumble) {
        return true;
    }

    if (usbh_xid_rumble(joystick->hwdata->xid_dev, low_frequency_rumble, high_frequency_rumble) != USBH_OK) {
        return false;
    }

    joystick->hwdata->current_rumble[0] = low_frequency_rumble;
    joystick->hwdata->current_rumble[1] = high_frequency_rumble;
    return false;
}

static bool SDL_XBOX_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    (void)joystick;
    (void)left_rumble;
    (void)right_rumble;
    return SDL_Unsupported();
}

static bool SDL_XBOX_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    (void)joystick;
    (void)red;
    (void)green;
    (void)blue;
    return SDL_Unsupported();
}

static bool SDL_XBOX_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    (void)joystick;
    (void)data;
    (void)size;
    return SDL_Unsupported();
}

static bool SDL_XBOX_JoystickSetSensorsEnabled(SDL_Joystick *joystick, bool enabled)
{
    (void)joystick;
    (void)enabled;
    return SDL_Unsupported();
}

static void SDL_XBOX_JoystickUpdate(SDL_Joystick *joystick)
{
    Sint16 wButtons, axis;
    Sint32 hat = SDL_HAT_CENTERED;
    XINPUT_GAMEPAD xpad;

    if (joystick == NULL || joystick->hwdata == NULL || joystick->hwdata->xid_dev == NULL) {
        return;
    }

    Uint64 timestamp = SDL_GetTicksNS();
    Uint8 button_data[MAX_PACKET_SIZE];

    KIRQL prev_irql = KeRaiseIrqlToDpcLevel();
    SDL_memcpy(button_data, joystick->hwdata->raw_data, MAX_PACKET_SIZE);
    KfLowerIrql(prev_irql);

    // FIXME. Steel Battalion and XREMOTE should be parsed differently.
    if (parse_input_data(joystick->hwdata->xid_dev, &xpad, button_data)) {
        wButtons = xpad.wButtons;

        // HAT
        if (wButtons & XINPUT_GAMEPAD_DPAD_UP)
            hat |= SDL_HAT_UP;
        if (wButtons & XINPUT_GAMEPAD_DPAD_DOWN)
            hat |= SDL_HAT_DOWN;
        if (wButtons & XINPUT_GAMEPAD_DPAD_LEFT)
            hat |= SDL_HAT_LEFT;
        if (wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)
            hat |= SDL_HAT_RIGHT;
        if (hat != joystick->hats[0]) {
            SDL_SendJoystickHat(timestamp, joystick, 0, hat);
        }

        // DIGITAL BUTTONS
        const Sint32 btn_map[10][2] = {
            { SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A },
            { SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B },
            { SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X },
            { SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER },
            { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER },
            { SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK },
            { SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START },
            { SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB },
            { SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB }
        };

        for (Uint32 i = 0; i < (sizeof(btn_map) / sizeof(btn_map[0])); i++) {
            if (joystick->buttons[btn_map[i][0]] != ((wButtons & btn_map[i][1]) > 0))
                SDL_SendJoystickButton(timestamp, joystick, btn_map[i][0], (wButtons & btn_map[i][1]) ? true : false);
        }

        // TRIGGERS
        // LEFT TRIGGER (0-255 must be converted to signed short)
        if (xpad.bLeftTrigger != joystick->axes[2].value)
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, ((xpad.bLeftTrigger << 8) | xpad.bLeftTrigger) - (1 << 15));
        // RIGHT TRIGGER (0-255 must be converted to signed short)
        if (xpad.bRightTrigger != joystick->axes[5].value)
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, ((xpad.bRightTrigger << 8) | xpad.bRightTrigger) - (1 << 15));

        // ANALOG STICKS
        // LEFT X-AXIS
        axis = xpad.sThumbLX;
        if (axis != joystick->axes[0].value)
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, axis);
        // LEFT Y-AXIS
        axis = xpad.sThumbLY;
        if (axis != joystick->axes[1].value)
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, ~axis);
        // RIGHT X-AXIS
        axis = xpad.sThumbRX;
        if (axis != joystick->axes[3].value)
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, axis);
        // RIGHT Y-AXIS
        axis = xpad.sThumbRY;
        if (axis != joystick->axes[4].value)
            SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, ~axis);
    }
    return;
}

static void SDL_XBOX_JoystickClose(SDL_Joystick *joystick)
{
    JOY_DBGMSG("SDL_XBOX_JoystickClose:\n");
    if (joystick->hwdata == NULL)
        return;

    usbh_xid_rumble(joystick->hwdata->xid_dev, 0, 0);

    xid_dev_t *xid_dev = joystick->hwdata->xid_dev;
    xid_dev->user_data = NULL;
    if (xid_dev != NULL) {
        JOY_DBGMSG("Closing joystick:\n", joystick->hwdata->xid_dev->uid);
        JOY_DBGMSG("joystick player_index: %i\n", joystick->player_index);
    }
    SDL_free(joystick->hwdata);
    joystick->hwdata = NULL;
    return;
}

static void SDL_XBOX_JoystickQuit(void)
{
    JOY_DBGMSG("SDL_XBOX_JoystickQuit\n");
    usbh_install_xid_conn_callback(NULL, NULL);
    // We dont call usbh_core_deinit() here incase the user is using
    // the USB stack in other parts of their application other than game controllers.
}

static bool SDL_XBOX_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    (void)device_index;
    (void)out;
    return false;
}

SDL_JoystickDriver SDL_XBOX_JoystickDriver = {

    .Init = SDL_XBOX_JoystickInit,
    .GetCount = SDL_XBOX_JoystickGetCount,
    .Detect = SDL_XBOX_JoystickDetect,
    .IsDevicePresent = SDL_XBOX_JoystickIsDevicePresent,
    .GetDeviceName = SDL_XBOX_JoystickGetDeviceName,
    .GetDevicePath = SDL_XBOX_JoystickGetDevicePath,
    .GetDeviceSteamVirtualGamepadSlot = SDL_XBOX_JoystickGetDeviceSteamVirtualGamepadSlot,
    .GetDevicePlayerIndex = SDL_XBOX_JoystickGetDevicePlayerIndex,
    .SetDevicePlayerIndex = SDL_XBOX_JoystickSetDevicePlayerIndex,
    .GetDeviceGUID = SDL_XBOX_JoystickGetDeviceGUID,
    .GetDeviceInstanceID = SDL_XBOX_JoystickGetDeviceInstanceID,
    .Open = SDL_XBOX_JoystickOpen,
    .Rumble = SDL_XBOX_JoystickRumble,
    .RumbleTriggers = SDL_XBOX_JoystickRumbleTriggers,
    .SetLED = SDL_XBOX_JoystickSetLED,
    .SendEffect = SDL_XBOX_JoystickSendEffect,
    .SetSensorsEnabled = SDL_XBOX_JoystickSetSensorsEnabled,
    .Update = SDL_XBOX_JoystickUpdate,
    .Close = SDL_XBOX_JoystickClose,
    .Quit = SDL_XBOX_JoystickQuit,
    .GetGamepadMapping = SDL_XBOX_JoystickGetGamepadMapping
};

static Sint32 parse_input_data(xid_dev_t *xid_dev, PXINPUT_GAMEPAD controller, Uint8 *rdata)
{

    if (xid_dev == NULL) {
        return 0;
    }

    Uint16 wButtons = *((Uint16 *)&rdata[2]);
    controller->wButtons = 0;

    // Map digital buttons
    if (wButtons & (1 << 0))
        controller->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    if (wButtons & (1 << 1))
        controller->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (wButtons & (1 << 2))
        controller->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (wButtons & (1 << 3))
        controller->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (wButtons & (1 << 4))
        controller->wButtons |= XINPUT_GAMEPAD_START;
    if (wButtons & (1 << 5))
        controller->wButtons |= XINPUT_GAMEPAD_BACK;
    if (wButtons & (1 << 6))
        controller->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (wButtons & (1 << 7))
        controller->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;

    // Analog buttons are converted to digital
    if (rdata[4] > BUTTON_DEADZONE)
        controller->wButtons |= XINPUT_GAMEPAD_A;
    if (rdata[5] > BUTTON_DEADZONE)
        controller->wButtons |= XINPUT_GAMEPAD_B;
    if (rdata[6] > BUTTON_DEADZONE)
        controller->wButtons |= XINPUT_GAMEPAD_X;
    if (rdata[7] > BUTTON_DEADZONE)
        controller->wButtons |= XINPUT_GAMEPAD_Y;
    if (rdata[8] > BUTTON_DEADZONE)
        controller->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; // BLACK
    if (rdata[9] > BUTTON_DEADZONE)
        controller->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER; // WHITE

    // Map the left and right triggers
    controller->bLeftTrigger = rdata[10];
    controller->bRightTrigger = rdata[11];

    // Map analog sticks
    controller->sThumbLX = *((Sint16 *)&rdata[12]);
    controller->sThumbLY = *((Sint16 *)&rdata[14]);
    controller->sThumbRX = *((Sint16 *)&rdata[16]);
    controller->sThumbRY = *((Sint16 *)&rdata[18]);
    return 1;
}

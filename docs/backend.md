# Using your own backend

The backend links devices to accounts, supplies signed pet content and runs
conversations. Speech recognition, personality, memory and voice generation
run there, not on the ESP32. Official defaults connect to aipets.com.
This repository contains firmware, not a deployable backend.

## Build settings

| Setting | Purpose |
| --- | --- |
| `CONFIG_PET_VNEXT_CONTROL_ORIGIN` | HTTPS origin, without a path or trailing slash, such as `https://pets.example.com`. Default: `https://device.aipets.com`. Empty disables setup requests. |
| `CONFIG_PET_VNEXT_RELEASE_KEY_ID` | Identifier for the backend's release verification key. |
| `CONFIG_PET_VNEXT_RELEASE_PUBLIC_KEY` | P-256 public key: a 130-hex-digit uncompressed point beginning with `04`. Empty rejects signed pet releases. |

Create a defaults file containing these settings and pass it as the build
script's third argument:

```sh
tools/build-firmware.sh waveshare-esp32-s3-touch-lcd-1.85b \
  build/my-backend my-backend.defaults
```

Use a fresh build directory when changing defaults. Only the **public**
verification key belongs in firmware. Keep the signing private key and all
provider credentials on your backend; never add them to this repository.

The conversation WebSocket follows the same origin at `/v1/device` using
WSS. Changing the origin alone does not replace the aipets.com wording on
setup and link screens; adapt those screens and follow
[trademark requirements](../TRADEMARKS.md) when distributing modified builds.

## Integration surface

The implementation in this repository describes the accepted device protocol:

- [`pet_control_http.h`](../firmware/main/pet_control_http.h): setup, linking
  and control HTTP operations under `/v1/device/`.
- [`pet_session_wire.h`](../firmware/main/pet_session_wire.h): conversation
  messages, story state, events and sound cues.
- [`pet_release_v2.h`](../firmware/main/pet_release_v2.h): signed release
  metadata and validation.

A backend must implement the actual linking, authentication, signing and
capability negotiation expected by these modules; an arbitrary voice or chat
server cannot be substituted by changing the URL. Enable optional story
features only when the device advertises the matching capability.

The current epoch-8 `one-pet-10m-v5` layout accepts pet and sound content over
Wi-Fi but **does not update firmware over Wi-Fi**. Its firmware installation
path is USB ([getting started](getting-started.md#flash-and-migrate)).

Linking follows the [confirmation flow in the README](../README.md#linking-a-device):
the owner enters the device code on the backend and confirms the matching
account and verification number on the device.

# Using your own backend

The device works with one backend: the service that links it to an account,
installs pets and firmware, and runs conversations. Official builds use
aipets.com. Two build settings point the firmware at another backend.

A self-hosted AI Pets backend for Docker is coming soon.

## Build settings

| Setting | What it is | Official builds |
| --- | --- | --- |
| Backend origin, `CONFIG_PET_VNEXT_CONTROL_ORIGIN` | The backend's HTTPS origin, with no path or trailing slash, for example `https://pets.example.com`. Setup, linking, the device's control polling and pet and firmware installs use it. Left empty, the device makes no setup requests. | `https://device.aipets.com` |
| Release key, `CONFIG_PET_VNEXT_RELEASE_KEY_ID` and `CONFIG_PET_VNEXT_RELEASE_PUBLIC_KEY` | The ID and P-256 public key (the 130-hex-digit uncompressed point, starting `04`) of the key your backend signs pet and firmware releases with. The device installs only releases signed with it. Left empty, it accepts none. | The aipets.com release key |

Put your values in a defaults file and pass it to the build as its third
argument. It is applied last, so it overrides the official values:

```sh
cat > my-backend.defaults <<'EOF'
CONFIG_PET_VNEXT_CONTROL_ORIGIN="https://pets.example.com"
CONFIG_PET_VNEXT_RELEASE_KEY_ID="pets.example.com-release-1"
CONFIG_PET_VNEXT_RELEASE_PUBLIC_KEY="04<128 more hex digits>"
EOF
tools/build-firmware.sh waveshare-esp32-s3-touch-lcd-1.85b build/my-backend my-backend.defaults
```

Only the public key goes into the firmware. Keep the signing key on your
backend.

Conversations follow the backend origin too: the voice WebSocket at
`/v1/device` goes to your backend. The setup and link screens still name
aipets.com (for example `AIPETS.COM/LINK`) whatever the backend.

## What the backend speaks

The device↔cloud wire format is defined by the `device-protocol` package of
the AI Pets cloud backend (published with the self-hosted backend): HTTP calls under `/v1/device/`
for setup, linking and control, signed pet and firmware releases, and the
WebSocket for conversations.

Linking works as with aipets.com ([README](../README.md#linking-a-device)):
the device shows its code, the owner enters it on the backend, and the device
asks to confirm the account.

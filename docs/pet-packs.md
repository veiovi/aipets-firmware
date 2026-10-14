# Pet packs

A pet is an `.aipetframes` pack: 240 × 240 animation frames, timing and state
metadata, played by `firmware/components/frame_player`. The current
`one-pet-10m-v5` layout holds **one pet**, up to **10,739,712 bytes**. Flash
capacity does not remove the runtime's separate RAM and validation limits.

Official characters are not included or licensed for other use
([trademarks](../TRADEMARKS.md)). The public
[AI Pets Sprite Skill](https://github.com/veiovi/aipets-sprite-skill) provides
an authoring workflow for your own packs; validate its output against the
firmware version you use.

## Install a pet over USB

First confirm that the device already uses the current layout. Migrate
older layouts before installing a pet
([getting started](getting-started.md#flash-and-migrate)).

This tool only prepares files; it never opens a serial port:

```sh
python3 firmware/tools/pocket_install_images.py \
  --pack my-pet.aipetframes --output pet-images
```

It checks a complete format-2 pack, the approved flag, the size limit and
that it is not a move pack. The firmware performs full validation on boot.
The tool writes `pet-images/pet_journal.bin` and
`pet-images/pet-flash-plan.json`, including hashes and the exact write plan:
the journal at `0x3b0000`, and the pet at `0x5c0000`.

Inspect the plan and use its printed esptool arguments with your device port:

```sh
esptool.py --chip esp32s3 -p PORT write_flash <the printed arguments>
```

This replaces the installed pet and its inventory. It leaves the settings
and sound-bank regions untouched. There is only slot 0; the earlier
three-pet/swipe-to-switch instructions do not apply.

## Signed content and conversations

An unsigned USB pet can display animations but does not establish the signed
backend binding required for conversations. A compatible backend installs
signed pets over Wi-Fi, verified using the release public key configured in
the firmware ([backend settings](backend.md)).

Story-event packs and sound-bank content are separate from the installed pet.
The backend supplies authenticated event metadata and content hashes; missing
or invalid event content falls back to narration. The 2 MiB sound bank is
reserved for story sounds. Installing a pet with the USB tool does not
populate that bank or create backend story assignments.

Firmware updates always use USB in this layout, whether the pet was installed
over USB or Wi-Fi.

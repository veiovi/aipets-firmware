# Pet packs

A pet is an `.aipetframes` pack: 240 × 240 animation frames for idle, listening,
thinking and speaking, touch and shake reactions and the pet's own moments,
with their timing. `firmware/components/frame_player` plays it. The device
holds up to three pets.

The official AI Pets characters are not in this repository and are not
licensed for other use ([trademarks](../TRADEMARKS.md)). Make your own.

## Make a pet

The [AI Pets Sprite Skill](https://github.com/veiovi/aipets-sprite-skill) for
Codex turns a description into a reviewed pet: you approve its look, it
animates the speech, blinks, glances and reactions, and it compiles an
`.aipetframes` pack you can preview in the browser.

## Install pets over USB

`firmware/tools/pocket_install_images.py` prepares one to three packs for a
board that already runs this firmware. It needs only Python 3 and never opens
a port:

```sh
python3 firmware/tools/pocket_install_images.py \
  --pack my-pet.aipetframes --pack second-pet.aipetframes \
  --output pet-images
```

It checks each pack first: a complete 240 px pack (format 2), marked approved,
not a move pack, and at most 3,002,368 bytes. It then writes
`pet-images/pet_journal.bin`, the list of installed pets, and
`pet-images/pet-flash-plan.json`, and prints the esptool arguments: the list at
`0x730000` and the packs at `0x740000`, `0xa1f000` and `0xcfe000`.

```sh
esptool.py --chip esp32s3 -p PORT write_flash <the printed arguments>
```

The packs replace the pets that were on the device. Wi-Fi and the link stay.
On the next boot the firmware checks each pack against the list's SHA-256 and
shows the first one; `--active 1` or `--active 2` starts with another. Swipe to
switch.

## What USB pets can't do

Pets installed over USB are unsigned. They play every animation, but:

- **They don't talk.** Only a pet that the linked backend installed and signed
  talks, with its own voice and memory.
- **The device takes no firmware update over Wi-Fi** while a USB pet is on it.
  Update it over USB ([getting started](getting-started.md#flash)), or replace
  the pets with ones installed from the backend.

A backend installs signed pets over Wi-Fi. The firmware verifies them with the
release key it was built with ([backend settings](backend.md)).

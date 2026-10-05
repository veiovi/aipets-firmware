# Sound cues

The firmware embeds these 18 cues (`main/CMakeLists.txt`) as signed 16-bit
little-endian mono PCM at 24 kHz. `manifest.json` records where each one comes
from and its SHA-256.

| Cues | Source | License |
| --- | --- | --- |
| `connect`, `start`, `stop`, `complete`, `cancel`, `retry`, `error`, `gesture_shake`, `gesture_pop` | [UISFX](https://uisfx.com/) 0.4.0, `minimal` feel ([repository](https://github.com/romainsimon/uisfx)) | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) |
| `wake`, `no_voice`, `settings_open`, `settings_close`, `face_swipe_0` … `face_swipe_4` | Tones written by `tools/generate_sfx_tones.py` | Apache-2.0 |

Official AI Pets devices play recorded sounds for the second group. Those
recordings are not part of this repository.

## When each cue plays

| Interaction | Cue |
| --- | --- |
| Boot, once audio is ready | `wake` |
| Successful cloud connection | `connect` |
| Start listening / finish recording | `start` / `stop` |
| Accepted tap, saved setting or long press | `gesture_pop` |
| Swipe to another pet (the five cues take turns) | `face_swipe_0` … `face_swipe_4` |
| Quick settings opened / closed with Done | `settings_open` / `settings_close` |
| A shake that reached the pet | `gesture_shake` |
| Installation complete | `complete` |
| Cancel / retry | `cancel` / `retry` |
| No speech heard / any other failure | `no_voice` / `error` |

`pet_sfx.c` plays cues at 70% of the master volume on a low-priority task, so
speech and the microphone always come first. The effects switch in the menu
silences everything except the conversation cues.

## Use your own sound

```sh
python3 -m pip install numpy soundfile
python3 tools/import_custom_sfx.py --cue wake --input path/to/sound.mp3
```

The importer keeps up to four seconds, converts the sound to the cue format,
keeps your original under `custom_sources/` and records both in
`manifest.json`. Only ship sounds you are allowed to share. A different length
changes the firmware size, and `firmware/tests/test_sound_cues.py` checks the
total.

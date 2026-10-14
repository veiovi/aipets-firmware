/* The ten loops pet_atmosphere.c links, under the symbols ESP-IDF's
 * EMBED_FILES gives them on the device, from ATMOSPHERE_DIR/<name>.adpcm
 * (test_playback_tail.py writes test loops there), and the sound bank's test
 * sounds (pet_audio_host.c). */
#ifndef ATMOSPHERE_DIR
#error "Build with -DATMOSPHERE_DIR=\"<directory of <name>.adpcm>\""
#endif

#ifdef __APPLE__
#define READ_ONLY ".pushsection __DATA,__const\n"
#else
#define READ_ONLY ".pushsection .rodata\n"
#endif

#define EMBED(name) __asm__(READ_ONLY \
    ".globl _binary_" #name "_adpcm_start\n_binary_" #name "_adpcm_start:\n" \
    ".incbin \"" ATMOSPHERE_DIR "/" #name ".adpcm\"\n" \
    ".globl _binary_" #name "_adpcm_end\n_binary_" #name "_adpcm_end:\n" \
    ".popsection\n");

EMBED(calm)
EMBED(forest)
EMBED(water)
EMBED(night)
EMBED(cave)
EMBED(storm)
EMBED(danger)
EMBED(battle)
EMBED(magic)
EMBED(victory)
EMBED(dice)
EMBED(hit)
EMBED(campfire)

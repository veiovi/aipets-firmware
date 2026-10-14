idf_component_register(
    SRCS "src/frame_player.c" "src/frame_director.c" "src/frame_actions.c" "src/frame_display.c"
         "src/frame_codec.c" "src/frame_stream.c" "vendor/miniz/miniz_tinfl.c"
    INCLUDE_DIRS "include"
)

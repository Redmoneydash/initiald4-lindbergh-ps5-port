#!/bin/bash
cd .
export LINDBERGH_WINDOW=1
export LINDBERGH_AUDIO=1
export LINDBERGH_APP0=/path/to/id4
export LINDBERGH_FRAME_DUMP=/tmp/id4frames
export LINDBERGH_FF_GAMEPROG=1
#export LINDBERGH_NO_FBO=1
exec ./id4 /path/to/id4/id4.elf

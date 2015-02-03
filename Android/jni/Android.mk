LOCAL_PATH := $(call my-dir)/..

include $(CLEAR_VARS)

LOCAL_MODULE    := puzzles
LOCAL_CFLAGS    := -DSLOW_SYSTEM -DANDROID -DSTYLUS_BASED -DNO_PRINTING -DCOMBINED
LOCAL_SRC_FILES := android.c blackbox.c boats.c bridges.c combi.c cube.c divvy.c dominosa.c drawing.c dsf.c fifteen.c filling.c flip.c flood.c galaxies.c grid.c guess.c inertia.c keen.c latin.c laydomino.c lightup.c list.c loopgen.c loopy.c magnets.c malloc.c map.c maxflow.c midend.c mines.c misc.c net.c netslide.c obfusc.c pattern.c pearl.c pegs.c penrose.c random.c range.c rect.c rome.c salad.c samegame.c signpost.c singles.c sixteen.c slant.c solo.c tdq.c tents.c towers.c tree234.c twiddle.c undead.c unequal.c unruly.c untangle.c version.c
LOCAL_DISABLE_FORMAT_STRING_CHECKS := true

include $(BUILD_SHARED_LIBRARY)

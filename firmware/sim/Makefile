# Desktop renderer: draws the player's screens with the real ui.c and writes PNGs to out/.
# Needs gcc, python3 with Pillow, and LVGL in ../managed_components (run any firmware build once first).
#   make            render both boards
#   make strip      render one board
L     := ../managed_components/lvgl__lvgl
M     := ../main
SRCS  := $(shell find $(L)/src -name '*.c' 2>/dev/null)
OBJS  := $(patsubst $(L)/src/%.c,obj/%.o,$(SRCS))
CFLAGS := -O1 -I. -Istub -I$(L) -DLV_CONF_INCLUDE_SIMPLE -w
APP   := sim.c $(M)/ui.c $(M)/kp_util.c $(wildcard $(M)/fonts/*.c)

all: strip cube

liblvgl.a: $(OBJS)
	@test -n "$(SRCS)" || (echo "LVGL not found in $(L): build the firmware once"; false)
	ar rcs $@ $^

obj/%.o: $(L)/src/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@gcc $(CFLAGS) -c $< -o $@

sim_cube: $(APP) liblvgl.a
	gcc $(CFLAGS) -I$(M) -DCONFIG_KP_BOARD_CUBE=1 -DCONFIG_KP_BOARD_NAME='"cube"' -DBOARD='"cube"' \
	    -include stub/sdkconfig.h $(APP) liblvgl.a -lm -o $@

sim_strip: $(APP) liblvgl.a
	gcc $(CFLAGS) -I$(M) -DCONFIG_KP_BOARD_STRIP=1 -DCONFIG_KP_BOARD_NAME='"strip"' -DBOARD='"strip"' \
	    -include stub/sdkconfig.h $(APP) liblvgl.a -lm -o $@

cube strip: %: sim_%
	@mkdir -p out
	./sim_$@
	python3 -c "import glob; from PIL import Image; [Image.open(f).save(f[:-4] + '.png') for f in glob.glob('out/$@_*.ppm')]"

.PHONY: all cube strip

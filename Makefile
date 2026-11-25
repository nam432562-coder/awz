BUILD_DIR = build
include $(N64_INST)/include/n64.mk

all: after_world_zero.z64

OBJS = $(BUILD_DIR)/main.o

after_world_zero.z64: N64_ROM_TITLE = "AFTER WORLD ZERO"

$(BUILD_DIR)/after_world_zero.elf: $(OBJS)

clean:
	rm -rf $(BUILD_DIR) *.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean

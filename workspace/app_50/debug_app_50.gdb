# debug_app_50.gdb - app_50 (VD6283TX x2 on I2C) on U575
#
#   ./prog.sh -s -a 2          # flash first
#   arm-none-eabi-gdb -x workspace/app_50/debug_app_50.gdb
#
# Breakpoints on the entry of the three tasks, plus the two places where a
# sensor bring-up can fail. The measurement tasks park on a semaphore, so BP1
# fires at boot for both while BP2 only fires after "lux start <n>".

file elfs/U575/u575_app_50.elf

target remote :3333
monitor reset halt
load

# Software breakpoints: the FPB unit has too few hardware slots for this many.
set remote hardware-breakpoint-limit 0
set pagination off

# --- BP1: measurement task entry (both tasks, at boot) --------------------
# idx 0 = sensor A, idx 1 = sensor B. Confirms both tasks were created and
# reached their entry point with a sane context.
b ca_os_app_50.c:485
commands
  silent
  printf "\n[TASK lux_%c] entry: idx=%u bus=I2C%u up=%d\n", (idx==0)?'a':'b', idx, s->bus, s->up
  printf "            semaphore='%s' count=%u\n", s->run_name, s->run.count
  continue
end

# --- BP2: bring-up, after the semaphore was posted ------------------------
# Only reached after "lux start <n>". If the DEVICE_ID read fails, BP3 fires.
b ca_os_app_50.c:494
commands
  silent
  printf "\n[CONFIG %s] starting bring-up on I2C%u (addr 0x%02X 7-bit)\n", s->label, s->bus, 0x20
  continue
end

# --- BP3: the DEVICE_ID read itself ---------------------------------------
# The single most informative point: whether the part answers at all.
b ca_os_app_50.c:383
commands
  silent
  printf "[PROBE %s] read DEVICE_ID on I2C%u -> ", s->label, s->bus
  next
  printf "id=0x%02X rev=0x%02X (expected id=0x70)\n", id[0], id[1]
  continue
end

# --- BP4: print task entry -------------------------------------------------
b ca_os_app_50.c:622
commands
  silent
  printf "\n[TASK lux_pr] entry: draining the queue\n"
  continue
end

# --- Fault catcher ---------------------------------------------------------
# The BusFault that hunted this application down came from the scheduler
# walking a corrupted context, far from the code that corrupted it. Stopping
# inside the handler keeps the frame that caused it.
b HardFault_Handler
commands
  printf "\n*** HARD FAULT — inspect with:\n"
  printf "      bt\n"
  printf "      p/x *(unsigned*)0xE000ED28   @ CFSR\n"
  printf "      p/x *(unsigned*)0xE000ED38   @ BFAR\n"
  printf "      p g_task_list.size\n"
end

# --- Handy ----------------------------------------------------------------
define sensors
  printf "A: bus=I2C%u up=%d present=%d running=%d fail=%d samples=%u errors=%u\n", \
         s_sensor[0].bus, s_sensor[0].up, s_sensor[0].present, s_sensor[0].running, \
         s_sensor[0].fail, s_sensor[0].samples, s_sensor[0].errors
  printf "B: bus=I2C%u up=%d present=%d running=%d fail=%d samples=%u errors=%u\n", \
         s_sensor[1].bus, s_sensor[1].up, s_sensor[1].present, s_sensor[1].running, \
         s_sensor[1].fail, s_sensor[1].samples, s_sensor[1].errors
end
document sensors
Print the state of both sensor contexts.
end

define tasks
  printf "tasks in list: %u\n", g_task_list.size
end
document tasks
Print how many tasks the scheduler is actually walking.
end

printf "\napp_50 debug session. Commands: 'sensors', 'tasks'\n\n"
continue

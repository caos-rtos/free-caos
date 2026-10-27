# stm32_prg.sh - where STM32CubeProgrammer is installed. Sourced, not run.
#
# STM32_PRG_PATH must name the bin/ directory that holds STM32_Programmer_CLI
# (the variable ST's own scripts use), e.g. in ~/.bashrc:
#
#   export STM32_PRG_PATH=/opt/st/STM32Cube/STM32CubeProgrammer/bin
#
# No default: a guessed path that is wrong on this machine would fail later,
# with a less clear message.
#
#   stm32_prg_cli
#     stdout: the full path of STM32_Programmer_CLI
#     return: 0 = found, 1 = not set or not there (message on stderr)

stm32_prg_cli() {
    if [ -z "${STM32_PRG_PATH:-}" ]; then
        echo "ERROR: STM32_PRG_PATH is not set." >&2
        echo "       Export the bin/ directory of STM32CubeProgrammer, e.g.:" >&2
        echo "       export STM32_PRG_PATH=/opt/st/STM32Cube/STM32CubeProgrammer/bin" >&2
        # sudo resets the environment: the user's export does not get here
        [ -n "${SUDO_USER:-}" ] && \
            echo "       Under sudo: sudo --preserve-env=STM32_PRG_PATH <command>" >&2
        return 1
    fi
    local cli="${STM32_PRG_PATH%/}/STM32_Programmer_CLI"
    if [ ! -x "$cli" ]; then
        echo "ERROR: $cli not found or not executable (check STM32_PRG_PATH)." >&2
        return 1
    fi
    echo "$cli"
}

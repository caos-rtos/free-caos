// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * vd6283_ctrl.h — VD6283TX types, register map and pure computation.
 *
 * Everything here is arithmetic: no I2C, no side effects on the part. The
 * split is deliberate - these are the pieces that can be reasoned about, and
 * checked, without a sensor on the bench.
 *
 *   vd6283_ctrl.*  this file: register map, sample and context types, the OTP
 *                  bit unpacking and calibration maths, the autogain solver,
 *                  saturation and normalisation.
 *   vd6283_i2c.*   everything that talks to the part over I2C.
 *   ca_os_main_app_50.c   tasks, CLI, menu and printing.
 */

#ifndef VD6283_CTRL_H
#define VD6283_CTRL_H

#include "caos.h"

#if EN_I2C

/* ============================================================================
 * PER-MCU MAPPING — the two I2C instances carrying one sensor each
 *
 * Only this block changes between boards; everything below uses the labels.
 * The pins are NOT set here: ca_i2c_hw_configure_gpio() in
 * hw/<family>/drivers/i2c_<family>.c programs them per instance number. They
 * are repeated in the text only to describe the wiring.
 *
 * ⚠️ "I2C1 and I2C2" is not portable across these boards. I2C1 lands on the
 * blue LD2 on the U575 and on the F446, and on the LPUART1 VCP on the H563 —
 * a LED driver or a UART on an open-drain pulled-up SDA line is a wiring
 * fault, not a cosmetic one. The instances below are the ones each board
 * actually brings out, taken from the same tables ex22 was fixed against.
 * ========================================================================== */

#if MCU_ID == 753
/* NUCLEO-H753ZI (MB1364): I2C1 PB8/PB9, I2C2 PB10/PB11. */
#define I2C_BUS_A       1U
#define I2C_BUS_B       2U
#define I2C_A_ALT       0U
#define I2C_B_ALT       0U
#define BUS_A_TXT       "I2C1 SCL:PB8  CN7-2   SDA:PB9  CN7-4"
#define BUS_B_TXT       "I2C2 SCL:PB10 CN10-32 SDA:PB11 CN10-34"
#define WIRE_A_SCL      "PB8  CN7-2   (bus A SCL) ---- sensor A SCL"
#define WIRE_A_SDA      "PB9  CN7-4   (bus A SDA) ---- sensor A SDA"
#define WIRE_B_SCL      "PB10 CN10-32 (bus B SCL) ---- sensor B SCL"
#define WIRE_B_SDA      "PB11 CN10-34 (bus B SDA) ---- sensor B SDA"
#define WIRE_A_PORT     CA_GPIO_PORT_B
#define WIRE_A_SCL_PIN  CA_GPIO_PIN_8
#define WIRE_A_SDA_PIN  CA_GPIO_PIN_9
#define WIRE_B_PORT     CA_GPIO_PORT_B
#define WIRE_B_SCL_PIN  CA_GPIO_PIN_10
#define WIRE_B_SDA_PIN  CA_GPIO_PIN_11
/* Power, from the Zio connectors (Nucleo-144 standard layout):
 * CN8-7 = 3V3, CN8-11 and CN8-13 = GND. */
#define PIN_3V3_A       "CN8-7"
#define PIN_GND_A       "CN8-11"
#define PIN_3V3_B       "CN8-7"
#define PIN_GND_B       "CN8-13"

#elif MCU_ID == 575
/* NUCLEO-U575ZI-Q (MB1549), pins from UM2861 Tables 15, 16, 17 and 18.
 * I2C1 is skipped here: the U5 driver programs it on PB6/PB7 and PB7 is the
 * blue LD2, driven through a transistor (UM2861 §6.4) — a LED driver on an
 * open-drain pulled-up SDA line. (DS13737 Table 27 does map I2C1 onto PB8/PB9
 * in AF4 as well, which is what CN7-2/CN7-4 bring out, but reaching it would
 * need an alternate pin map in i2c_u5xx.c that does not exist yet.)
 * I2C4 PF14/PF15 AF4 is used instead: CN10-8 (D4) and CN10-12 (D2). */
#define I2C_BUS_A       4U
#define I2C_BUS_B       2U
#define I2C_A_ALT       0U
#define I2C_B_ALT       0U
#define BUS_A_TXT       "I2C4 SCL:PF14 CN10-8  SDA:PF15 CN10-12"
#define BUS_B_TXT       "I2C2 SCL:PF1  CN9-19  SDA:PF0  CN9-21"
#define WIRE_A_SCL      "PF14 CN10-8  (bus A SCL) ---- sensor A SCL"
#define WIRE_A_SDA      "PF15 CN10-12 (bus A SDA) ---- sensor A SDA"
#define WIRE_B_SCL      "PF1  CN9-19  (bus B SCL) ---- sensor B SCL"
#define WIRE_B_SDA      "PF0  CN9-21  (bus B SDA) ---- sensor B SDA"
#define WIRE_A_PORT     CA_GPIO_PORT_F
#define WIRE_A_SCL_PIN  CA_GPIO_PIN_14
#define WIRE_A_SDA_PIN  CA_GPIO_PIN_15
#define WIRE_B_PORT     CA_GPIO_PORT_F
#define WIRE_B_SCL_PIN  CA_GPIO_PIN_1
#define WIRE_B_SDA_PIN  CA_GPIO_PIN_0
/* Power, from the Zio connectors (UM2861 Table 16 and Table 17):
 * CN8-7 = 3V3, CN8-11 and CN8-13 = GND, CN9-23 = GND, CN10-5 = GND. */
#define PIN_3V3_A       "CN8-7"
#define PIN_GND_A       "CN8-11"
#define PIN_3V3_B       "CN8-7"
#define PIN_GND_B       "CN8-13"

#elif MCU_ID == 563
/* NUCLEO-H563ZI (MB1404), pins from UM3115 Tables 21 and 23. NOT the
 * reference-manual defaults: PB6/PB7 are the LPUART1 VCP pair, PB10 is CN10-32
 * as TIM2_CH3, and PB11 reaches no Zio connector at all. */
#define I2C_BUS_A       1U
#define I2C_BUS_B       2U
#define I2C_A_ALT       0U
#define I2C_B_ALT       0U
#define BUS_A_TXT       "I2C1 SCL:PB8 CN7-2   SDA:PB9 CN7-4"
#define BUS_B_TXT       "I2C2 SCL:PF1 CN9-19  SDA:PF0 CN9-21"
#define WIRE_A_SCL      "PB8 CN7-2   (bus A SCL) ---- sensor A SCL"
#define WIRE_A_SDA      "PB9 CN7-4   (bus A SDA) ---- sensor A SDA"
#define WIRE_B_SCL      "PF1 CN9-19  (bus B SCL) ---- sensor B SCL"
#define WIRE_B_SDA      "PF0 CN9-21  (bus B SDA) ---- sensor B SDA"
#define WIRE_A_PORT     CA_GPIO_PORT_B
#define WIRE_A_SCL_PIN  CA_GPIO_PIN_8
#define WIRE_A_SDA_PIN  CA_GPIO_PIN_9
#define WIRE_B_PORT     CA_GPIO_PORT_F
#define WIRE_B_SCL_PIN  CA_GPIO_PIN_1
#define WIRE_B_SDA_PIN  CA_GPIO_PIN_0
/* Power, from the Zio connectors (Nucleo-144 standard layout):
 * CN8-7 = 3V3, CN8-11 and CN8-13 = GND. */
#define PIN_3V3_A       "CN8-7"
#define PIN_GND_A       "CN8-11"
#define PIN_3V3_B       "CN8-7"
#define PIN_GND_B       "CN8-13"

#elif MCU_ID == 446
/* NUCLEO-F446ZE (MB1137), pins from UM1974 Table 18. Both buses need the
 * ALTERNATE map: the F4 driver defaults put I2C1 SDA on the blue LD2 (PB7)
 * and I2C2 SDA on PB11, which reaches no connector. */
#define I2C_BUS_A       1U
#define I2C_BUS_B       2U
#define I2C_A_ALT       1U   /* I2C1 -> PB8/PB9 */
#define I2C_B_ALT       1U   /* I2C2 -> PF1/PF0 */
#define BUS_A_TXT       "I2C1 SCL:PB8 CN7-2   SDA:PB9 CN7-4"
#define BUS_B_TXT       "I2C2 SCL:PF1 CN9-19  SDA:PF0 CN9-21"
#define WIRE_A_SCL      "PB8 CN7-2   (bus A SCL) ---- sensor A SCL"
#define WIRE_A_SDA      "PB9 CN7-4   (bus A SDA) ---- sensor A SDA"
#define WIRE_B_SCL      "PF1 CN9-19  (bus B SCL) ---- sensor B SCL"
#define WIRE_B_SDA      "PF0 CN9-21  (bus B SDA) ---- sensor B SDA"
#define WIRE_A_PORT     CA_GPIO_PORT_B
#define WIRE_A_SCL_PIN  CA_GPIO_PIN_8
#define WIRE_A_SDA_PIN  CA_GPIO_PIN_9
#define WIRE_B_PORT     CA_GPIO_PORT_F
#define WIRE_B_SCL_PIN  CA_GPIO_PIN_1
#define WIRE_B_SDA_PIN  CA_GPIO_PIN_0
/* Power, from the Zio connectors (Nucleo-144 standard layout):
 * CN8-7 = 3V3, CN8-11 and CN8-13 = GND. */
#define PIN_3V3_A       "CN8-7"
#define PIN_GND_A       "CN8-11"
#define PIN_3V3_B       "CN8-7"
#define PIN_GND_B       "CN8-13"

#else
/* Other families: keep the driver defaults and check the pins in
 * hw/<family>/drivers/i2c_<family>.c before wiring anything. */
#define I2C_BUS_A       1U
#define I2C_BUS_B       2U
#define I2C_A_ALT       0U
#define I2C_B_ALT       0U
#define BUS_A_TXT       "I2C1 (see i2c_<family>.c for the pins)"
#define BUS_B_TXT       "I2C2 (see i2c_<family>.c for the pins)"
#define WIRE_A_SCL      "bus A SCL ---- sensor A SCL"
#define WIRE_A_SDA      "bus A SDA ---- sensor A SDA"
#define WIRE_B_SCL      "bus B SCL ---- sensor B SCL"
#define WIRE_B_SDA      "bus B SDA ---- sensor B SDA"
/* Power pins unknown for this board: not a Nucleo-144 Zio layout. */
#define PIN_3V3_A       "3V3"
#define PIN_GND_A       "GND"
#define PIN_3V3_B       "3V3"
#define PIN_GND_B       "GND"
#endif

/* ============================================================================
 * VD6283TX REGISTER MAP  (DS13735 rev 3 §4.2, Table 10)
 * ========================================================================== */

#define VD6283_ADDR         0x20u   /* 7-bit. §3.4.1 gives 0x40 as the 8-bit
                                     * write address, "or 0x20 in 7 bits". The
                                     * caOS I2C API takes the 7-bit form and
                                     * appends the R/W bit itself. */

#define VD6283_DEVICE_ID    0x00u   /* reset value 0x70 */
#define VD6283_REVISION_ID  0x01u   /* reset value 0xBD */
#define VD6283_INTERRUPT_CTRL 0x02u
#define VD6283_ALS_CTRL     0x03u
#define VD6283_ALS_PERIOD   0x04u
/* Channel data: 24 bits big-endian at 0x06 + 4c, exactly as §4.3.6 says
 * (DATA_H = [23:16] at 0x06, DATA_M = [15:8] at 0x07, DATA_L = [7:0] at 0x08).
 * Each channel owns a group of FOUR registers; the first, 0x05 + 4c, is
 * padding and reads 0. That is why the stride is 4 and the base is 0x06.
 *
 * ⚠️ This used to be 0x05 + 4c, on the theory that the ST driver map
 *     CHANNELx(c) = 0x05 + 4c   CHANNELx_MM/_LM/_LL = 0x06/0x07/0x08 + 4c
 * meant the value STARTED at 0x05 and the datasheet had simply omitted its
 * most significant byte. It does not. STALS_GetAlsValues takes the first three
 * bytes of the group - pad, H, M - so what ST reports is the 16-bit format
 * (H:M) carried on the 24-bit scale, which is why STALS_GetAlsValues shifts it
 * up by 8 and why STALS_LIB_normalize_als_for_xyz divides CountValue by 256 to
 * get back to counts.
 *
 * Reading from 0x05 therefore did not gain a byte, it LOST one: the low byte
 * never entered a reading and every count was quantised to multiples of 256 on
 * the 24-bit scale. Measured on HW 2026-08-31 (`lux bytes`, both sensors, 72
 * reads): the pad byte was 0 every time, and the low byte was non-zero every
 * time. Over six frames of a slowly rising light all five lit channels drifted
 * monotonically in the 24-bit value while blue and IR sat frozen on a single
 * 16-bit count - the resolution this cost.
 *
 * §3.3.3 says it plainly: the low byte "is useful for improving sensing
 * resolution" at low light. Which is where the dark channel lives. */
#define VD6283_CH_DATA(c)   (0x06u + 4u * (c))

/* The padding byte before each group. Only the raw-byte dump reads it, to keep
 * checking the assumption above rather than trusting this comment. */
#define VD6283_CH_PAD(c)    (0x05u + 4u * (c))
#define VD6283_CH1_DATA_H   VD6283_CH_DATA(0)
#define VD6283_CH2_DATA_H   VD6283_CH_DATA(1)   /* visible, or dark when the
                                                 * photodiode selection is
                                                 * switched (SEL_PD below) */
#define VD6283_EXPOSURE_M   0x1Du   /* 0x1D..0x1E, EXTIME[9:0] */
#define VD6283_GAIN_CH1     0x25u   /* 0x25..0x2A, one per channel */
#define VD6283_CH6_ENABLE   0x2Du
#define VD6283_CH_ENABLE    0x2Eu
#define VD6283_AC_MODE_CTRL 0x31u

/* Registers the datasheet does not list, from the ST driver map
 * (VD6283_map.h). They are what §4.1 means by "access the registers only
 * through the ST driver": without them the part answers on I2C but never
 * produces correct counts. */
#define VD6283_AC_CLAMP_EN  0x2Fu
#define VD6283_AC_PEDESTAL  0x32u
#define VD6283_DC_CLAMP_EN  0x30u

/* DC_CLAMP_EN carries one bit per DC channel, CH1..CH5 (the code indexes them
 * 0..4). clamp_enable() in the ST driver writes all five.
 *
 * ⚠️ THE CLAMP IS NOT WHY THE DARK READS ZERO. That was the hypothesis this
 * switch was built to test, and the measurement refuted it on 2026-08-31: with
 * channel 2 UNCLAMPED (0x1D) and the sensors covered, sensor A still read
 * exactly 0, and sensor B still read its ~270-count residue - the same values
 * as with the clamp on. Register addresses were checked against ST's
 * VD6283_map.h, so the write was reaching the right place. The comparison is
 * between two coverings of equal quality, so it holds whatever that quality
 * was.
 *
 * The plain reading of the evidence is the boring one: at 1.00x and 400 ms the
 * dark current of these channels is simply BELOW ONE COUNT. That is also what
 * Table 16 implies by specifying the dark count noise at 66x rather than at
 * unity gain. The ~270 on sensor B's channel 2 is a per-part offset, not a
 * temperature signal - it does not move with the clamp.
 *
 * The switch is kept because it is a real control and the negative result is
 * worth being able to reproduce. ⚠️ "off" departs from the ST driver, whose
 * set_output_dark_enable() writes SEL_PD and nothing else, so ST's own dark
 * output has the clamp active too; the channel's calibration may not survive
 * it. On by default, which is ST's configuration. */
#define DC_CLAMP_ALL        0x1Fu   /* CH1..CH5, what clamp_enable() writes */
#define DC_CLAMP_CH2_OFF    0x1Du   /* the same, minus channel 2 (bit 1) */
#define VD6283_OSC10M       0x3Du
#define VD6283_OSC10M_TRIM_M 0x3Eu
#define VD6283_OSC10M_TRIM_L 0x3Fu
#define VD6283_OSC50K_TRIM  0x40u
#define VD6283_SPARE_0      0x71u
#define VD6283_SPARE_1      0x72u
#define VD6283_SPARE_0_IN_GPIO2  0x01u
#define VD6283_DITHER_CTRL  0x76u
#define VD6283_DITHER_STEP_VALUE    0x77u
#define VD6283_DITHER_STEP_DURATION 0x78u
#define VD6283_DITHER_STEP_NUMBER   0x79u

/* Oscillator trims. The ST driver reads per-part values from OTP; these are
 * its documented fallbacks (VD6283_DEFAULT_HF_TRIM / _LF_TRIM), used when the
 * OTP cannot be read. EXTIME is counted in cycles of the 10.24 MHz oscillator,
 * so an untrimmed clock makes every exposure - and thus every count - wrong by
 * the trim error. */
#define VD6283_HF_TRIM      0x0E3u
#define VD6283_LF_TRIM      0x07u

/* ── OTP: the per-part factory calibration ──────────────────────────────────
 *
 * None of these registers is in the datasheet; they come from VD6283_map.h.
 * The OTP holds a correction coefficient per channel plus the real oscillator
 * trims for THIS die. Without it every part is assumed nominal
 * (VD6283_DEFAULT_GAIN = 0x80, factor x1.000), which is what the ST driver
 * does when is_otp_usage_enable is false.
 *
 * Why it matters here: the coefficient spans x0.781..x1.219, so two parts can
 * differ by up to 56% on the same light. Readings stay repeatable on one
 * sensor but are NOT comparable between the two - which is the whole point of
 * this application. hf_trim compounds it: it corrects the clock that COUNTS
 * the exposure, so with the default the 400 ms window is nominal, not real. */
#define VD6283_OTP_CTRL1        0x58u
#define VD6283_OTP_STATUS       0x5Au
#define VD6283_OTP_DATA_READY   0x0Au   /* BIT(1) | BIT(3), both banks ready */
#define VD6283_OTP_BANK_0       0x5Bu   /* 8 bytes */
#define VD6283_OTP_BANK_1       0x63u   /* 8 bytes */
#define VD6283_OTP_POLL_MAX     100u    /* same bound as ST's otp_reset() */

/* Nominal coefficient: x1.000 in the 7-bit fixed point of apply_calfactor
 * (count * factor >> 7). Used per channel when the OTP is unavailable. */
#define VD6283_DEFAULT_GAIN     0x80u

/* Bit positions inside the 120-bit OTP, from otp_read_param_details(). */
#define OTP_BIT_GAIN(c)     ((c) * 8u)  /* 8 bits each, channels 0..5 */
#define OTP_BIT_HF_TRIM     51u         /* 9 bits */
#define OTP_BIT_LF_TRIM     116u        /* 4 bits */

/* ── Analog gain ────────────────────────────────────────────────────────────
 *
 * ⚠️ Register 0x25+c does NOT hold the gain: set_channel_gain() writes
 * `vref + 1`, the INDEX into this table. Writing a gain value there yields a
 * silently wrong gain - the part accepts any byte.
 *
 * The table is DESCENDING (index 0 is the largest gain), so raising the gain
 * means DECREASING the index. Values are 8.8 fixed point: 0x0100 = 1.00x.
 * AGAIN_1X (0x0D) is index 12 + 1, which is why unity gain is written as 0x0D. */
#define GAIN_TABLE_SIZE     15u
#define GAIN_IDX_1X         12u          /* GainRange[12] = 0x0100 = 1.00x */

/* Gain at start-up and after "reset to defaults".
 *
 * Unity. 10x was tried and saturated the clear channel in ordinary indoor
 * light: the part saturates well below the register width (§3.3.4 and Table
 * 16), so the headroom above 1x is smaller than 24 bits suggest. At 1x the six
 * channels stay on scale and the autogain can raise the gain when the light
 * actually calls for it.
 *
 * ⚠️ Unity is a bench default, NOT the setting for a threshold alarm. Two
 * reasons, both measured here:
 *
 *  - at 1x the dark channel sits at the bottom of the range (it read a single
 *    16-bit count before the readout was widened, ~170 normalised counts now),
 *    while Table 16 specifies the dark count noise at 66x. The dark is the
 *    thermal reference the light reading is corrected against, so it has to be
 *    resolved, not just present;
 *  - the autogain must be OFF and the gain fixed, and never on either side of
 *    33.33x across a run: that is where the OTP coefficient switches nibble
 *    (cal10/cal50, GAIN_CAL_SPLIT) and every absolute column steps by up to
 *    18% with no change in the light.
 *
 * Kept as its own name rather than reusing GAIN_IDX_1X: the default is a
 * choice that may change again, while "index of unity gain" is a fact about
 * the table that AGAIN_1X and gain_text() rely on. */
#define GAIN_IDX_DEFAULT    GAIN_IDX_1X  /* 1.00x */

/* Defined in vd6283_ctrl.c: one copy, or every translation unit gets its own. */
extern const u16 GAIN_VALUE[GAIN_TABLE_SIZE];

/* Human-readable, in thousandths of x (66.67x -> 66670). ca_vsnprintf has no
 * float, so every gain is printed as integer.fraction from this. */
extern const u32 GAIN_MILLI[GAIN_TABLE_SIZE];

/* The cal10/cal50 split of calfactor1050(): which nibble of the OTP byte is
 * the coefficient depends on the gain VALUE in use, not on the register code. */
#define GAIN_CAL_SPLIT      0x2154u

/* Per-channel photodiode selection. NOT in the datasheet register map: taken
 * from the ST driver (VD6283_map.h, `VD6283_SEL_PD_x(c) = 0x6B + c`), which is
 * what §4.1 means when it says the registers should be accessed through the ST
 * driver. Each register picks WHICH photodiodes of the 5x5 matrix feed that
 * channel; select_pd() programs {07,07,07,1f,0f,1f} at init, which is why the
 * clear channel (5 diodes) is the most sensitive and saturates first. */
#define VD6283_SEL_PD(c)    (0x6Bu + (c))

/* Channel 2 is the one that can be switched to the covered photodiodes:
 * set_output_dark_enable() writes 0x18 for dark and 0x07 to restore visible.
 * 0x07 = 00111 (3 diodes, visible), 0x18 = 11000 (the 2 black squares of
 * Figure 17). So the dark count is read from ALS_CH2_DATA like any other
 * channel - there is no dedicated data register, and visible and dark cannot
 * be measured in the SAME frame because they share channel 2. */
#define SEL_PD1_VISIBLE     0x07u
#define SEL_PD1_DARK        0x18u

#define VD6283_DEV_ID_VAL   0x70u

/* ALS_CTRL (§4.3.4) */
#define ALS_EN              (1u << 0)
#define ALS_CONT_MODE3      (3u << 1)   /* continuous, paced by ALS_PERIOD */

/* INTERRUPT_CTRL (§4.3.3) — the handshake that mode 3 waits for.
 * ⚠️ In mode 3 the part finishes measurement N and then STOPS: §3.3 says
 * "waits for the microcontroller handshake to carry on to measurement N+1
 * whilst I2C data are NOT refreshed until the next acknowledgment". Without
 * it every read returns the same frozen frame - the readings simply stop
 * responding to light, which is exactly how this shows up on the bench. */
#define INTR_ST             (1u << 1)   /* read-only: 1 = interrupt pending */
#define CLR_INTR            (1u << 0)   /* write 1 then 0 to acknowledge */

/* ALS_CHANNEL_ENABLE (§4.3.20): channels 1..5. Channel 6 has its own
 * register (§4.3.19) — enabling all six takes both writes. */
#define ALS_CH_ALL_1_5      0x1Fu
#define CH6_EN              (1u << 0)

/* AGAIN codes (§4.3.13). 0xD is 1x — unity gain, the quietest choice: analog
 * gain lifts the noise with the signal. */
#define AGAIN_1X            0x0Du

/* EXTIME: exposure = EXTIME[9:0] x 16384 / 10.24 MHz, i.e. steps of 1.6 ms up
 * to 0x3FF = 1.6368 s. See EXTIME_PLUS_ONE below for why there is no "+1"
 * here, against what §4.3.12 prints - and for what it was worth when this port
 * believed the formula instead (the claim that once stood in this comment, that
 * the difference "never affects a ratio", was the part that was wrong).
 *
 * 249 gives 398.4 ms. The maximum (0x3FF) is quieter still, but a full
 * sample is one exposure, so 1.6 s each meant ~1.6 s per reading
 * - too slow to watch the sensor respond to a hand moving over it. At ~400 ms a
 * sample takes ~1.6 s and stays well above the 113 ms threshold of §3.3.4,
 * which is what keeps the saturation ceiling at full scale rather than 910
 * counts. It also buys back headroom on clear, the channel that was clipping. */
#define EXTIME_STEPS        249u         /* -> 398.4 ms */
#define EXTIME_US_PER_STEP  1600u        /* 16384 / 10.24 MHz = 1.6 ms */

/* ⚠️ THE STEP COUNT IS NOT OFF BY ONE: exposure = EXTIME x 1.6 ms, flat.
 *
 * §4.3.12 prints the formula as "(EXTIME[9:0] + 1) x 16384/Fosc" and this port
 * believed it until 2026-09-02. It is wrong, and the same datasheet contradicts
 * it in three places - every one of them consistent with NO +1:
 *
 *   Table 16   "EXTIME = 0x001 to 0x047 (1.6 ms to 113.6 ms)"
 *              0x001 -> 1 x 1.6 = 1.6      (with +1 it would be 3.2)
 *              0x047 -> 71 x 1.6 = 113.6   (with +1, 115.2)
 *   Table 16   "EXTIME = 0x048 to 0x3FF (115.2 ms to 1.6 s)"
 *              0x048 -> 72 x 1.6 = 115.2   (with +1, 116.8)
 *   §4.3.12    "Default value = 80 ms" for a reset value of 0x0032
 *              50 x 1.6 = 80.0             (with +1, 81.6)
 *
 * ST's own driver agrees: set_exposure() in VD6283.c computes
 * steps = round(us/1600), caches us = steps * 1600, and clamps steps to [1,
 * 0x3FF] - i.e. it treats 0 as invalid, which only makes sense without the +1.
 * STALS_LIB_normalize_als_for_xyz() then normalises to 100800 us, which is
 * 63 steps exactly on this convention (and 62.0 on the other - not a round
 * number, which is a hint in itself).
 *
 * What it changed here: EXP now reads 398 ms where it read 400, the step
 * threshold of als_sat_level() is unaffected (Table 16 splits on the STEP
 * count, which is why it was written that way), and norm_count() lost the +1
 * from both sides of its ratio. The error was 1/(steps+1) - 0.4% at the
 * default exposure, but a factor of TWO at EXTIME=1 and 1.5x at EXTIME=2, so
 * any capture taken at a short exposure and normalised is affected.
 *
 * ⚠️ Captures taken before this date carry the old EXP column. The reading is
 * the same light; only the number printed for the exposure moved by one step.
 *
 * Kept as a macro so the question can be re-opened with one edit if a bench
 * measurement ever contradicts the three statements above: set it to 1u and
 * everything that depends on the convention follows. The experiment that
 * settles it is EXTIME=1 against EXTIME=3 under a steady light - counts in the
 * ratio 1:3 mean no +1, 1:2 means +1. */
#define EXTIME_PLUS_ONE     0u
#define EXTIME_UNITS(st)    ((u32)(st) + EXTIME_PLUS_ONE)

/* Conversion housekeeping after each EXTIME window (§3.2). */
#define CONV_TIME_MS        6u

/* ── Inter-measurement period (ALS_PERIOD, §4.3.5) ──────────────────────────
 *
 * The idle gap the part inserts between one measurement and the next. It has
 * been 0 since the beginning - back-to-back frames, EXTIME alone setting the
 * pace - which is right for a measurement bench and wrong for the thing this
 * application is meant to become: a threshold alarm does not need a reading
 * every 400 ms, and the frames it does not take are the cheapest power saving
 * available. ST's continuous example runs at 100 ms for the same reason.
 *
 * ⚠️ The period is NOT the sample interval, it is the GAP: the part measures
 * for EXTIME, then idles for ALS_PERIOD, so the interval is the sum. A period
 * of 0 keeps the old behaviour exactly.
 *
 * §4.3.5 gives LSB = 20.5 ms and max 5.22 s over the 8-bit field; Table 16
 * gives "IM step size 20 ms" and "20 ms to 5.12 s" for 1..256 steps. The two
 * disagree by 2.5%, which is the oscillator tolerance, and neither is worth
 * arguing about for an idle gap - the nominal 20.5 ms of the register
 * description is used here because it is the one attached to the register.
 *
 * ⚠️ Timing that WAITS on a frame must include this gap or it times out on
 * every sample: see FRAME_TIMEOUT_MS and SAMPLE_PERIOD_MS. */
#define IM_STEP_US          20500u
#define IM_STEPS_MAX        255u
#define IM_STEPS_TO_MS(p)   (((u32)(p) * IM_STEP_US) / 1000u)
#define IM_MS_TO_STEPS(ms)  ((((u32)(ms) * 1000u) + (IM_STEP_US / 2u)) / IM_STEP_US)

#define ALS_CHANNELS        6u

/* Widest value the 24-bit registers can carry. Used to clamp the calibrated
 * result, which the OTP coefficient can push past the ceiling. */
#define ALS_FULL_SCALE_24   0xFFFFFFu

/* ⚠️ SATURATION - the level a CHANNEL actually stops at. It depends on the
 * exposure, and Table 16 states it on BOTH scales:
 *
 *     EXTIME 0x001..0x047   24-bit: 233045 per step     16-bit: 910 per step
 *     EXTIME 0x048..0x3FF   24-bit: 16777215            16-bit: 65535
 *
 * The two columns are the same physical ceiling read at two resolutions, in
 * the ratio 1:256. This port reads the full 24 bits (see VD6283_CH_DATA), so
 * the 24-bit column is the one that applies.
 *
 * ⚠️ It used to use the 16-bit column, because it used to read the 16-bit
 * format without knowing it. Both halves were wrong and they cancelled, which
 * is exactly why it went unnoticed: channels pinned at "65535" and the ceiling
 * said 65535. The old note here - that the "based on a 16-bit ADC format"
 * remark described only the format, so 65535 applied "whatever we read" - was
 * false. Table 16 gives the 24-bit ceiling explicitly, in its own row.
 *
 * The threshold is on the STEP COUNT, not on milliseconds: Table 16 splits at
 * 0x047/0x048. Comparing 113 ms against an exposure derived with the (EXTIME+1)
 * formula put step 71 - still in the per-step band - on the wrong side by one.
 *
 * §3.3.4: the check must be done "on VD6283TX raw data", before the per-part
 * correction, because the coefficient scales a pinned value and carries it past
 * the ceiling (which is why a clipped vis reported 67582 and not 65535). */
#define ALS_SAT_STEP_COUNTS 233045u     /* per step, EXTIME <= 0x047 */
#define ALS_SAT_LONG_EXP    ALS_FULL_SCALE_24   /* EXTIME >= 0x048 */
#define ALS_SAT_STEP_THR    0x47u       /* last step of the per-step band */




/* ============================================================================
 * APPLICATION CONFIGURATION
 * ========================================================================== */

/* ── Normalising to a common reference ──────────────────────────────────────
 *
 * The count is proportional to gain x exposure, so a reading taken at 7.13x /
 * 369 ms cannot be compared with one at 2.50x / 392 ms as it stands - and the
 * autogain puts the two sensors on different settings whenever they see
 * different light, which is exactly when the comparison matters.
 *
 * Dividing by (gain x exposure) and scaling back to a fixed reference makes
 * every row comparable with every other, whatever the settings were when it
 * was taken. Measured on HW: raw counts made A and B look nearly equal, while
 * normalised they showed B receiving ~2.4x the light of A - which is the true
 * picture, and the reason the autogain had separated them in the first place.
 *
 * Reference: 1.00x and the default exposure, so a run that never moves off
 * those settings has NVIS == VIS. */
#define NORM_GAIN       0x0100u          /* 1.00x in 8.8 */
#define NORM_STEPS      EXTIME_STEPS     /* the reference exposure, in STEPS */

/* Full scale ON THE NORMALISED SCALE - the ceiling a normalised count is a
 * percentage of. NORM_STEPS is 249, past the 0x047 split of Table 16, so the
 * reference exposure sits in the flat band and the ceiling is the full 24 bits.
 *
 * ⚠️ It is a CONSTANT, and that is the whole point. A percentage taken against
 * the raw ceiling of the settings actually in force moves when the autogain
 * moves, so two rows holding the same light report different headroom - seen on
 * HW as 89% and 24% on consecutive rows whose NVIS differed by 1%. A percentage
 * of the unified scale is comparable between rows, between sensors and between
 * captures, which is what makes it a column worth having.
 *
 * It does NOT say how close the ADC is to clipping - that is a property of the
 * raw reading at its own settings, and it is reported separately by the
 * saturation marker. The two are different questions and now have different
 * answers. */
#define NORM_FULL_SCALE ALS_SAT_LONG_EXP

/* ── Photometry: lux and CCT from R, G, B ───────────────────────────────────
 *
 * Ported from ST's STALS_LIB (vd6283_drv/lib/VD6283_lib.c, driver v2.3.0) and
 * its als_cct_lux example. Three steps:
 *
 *   1. normalise each channel out of its gain and exposure, onto a fixed
 *      reference exposure:   norm[c] = (REF_US/expo_us) * count[c] / gain
 *   2. a 3x3 matrix takes (R,G,B) to CIE XYZ
 *   3. lux IS Y - STALS_LIB_compute_lux() literally discards X and Z - and CCT
 *      comes from the chromaticity (x,y) through McCamy's cubic.
 *
 * ⚠️ THIS IS THE ONE ROUTE TO A NUMBER IN LUX, AND IT DOES NOT USE VIS.
 * It reads R, G and B, which means it works unchanged while channel 2 is on
 * the covered photodiodes - the reconstruction of CAL_K_SCALE exists precisely
 * because VIS was unavailable there, and for photometry it is not needed at
 * all. A threshold expressed in lux is a statement about light; a threshold in
 * counts is a statement about counts, gain and exposure at once.
 *
 * ⚠️ AND IT IS NOT A SUM. The matrix rows carry NEGATIVE coefficients (-0.14,
 * -0.03, -0.12, -0.55), so no k(R+G+B) reproduces Y for any k: the colour
 * channels overlap in wavelength and the matrix subtracts the overlap back
 * out. Row 3 in particular is mostly -R +G +B.
 *
 * ⚠️ THE MATRIX IS A CALIBRATION, NOT A CONSTANT OF THE PART. These are the
 * coefficients ST ships in als_cct_lux.c, measured on their own module with
 * its own cover glass. They will give the right ORDER and excellent
 * RELATIVE behaviour on this bench, and an absolute lux that is only as good
 * as the optical stack matching ST's. Calibrating them here means a reference
 * luxmeter, which this bench does not have; until then read the number as
 * "lux, uncalibrated" and trust ratios rather than absolutes.
 *
 * Held x1e6 as integers: the kernel saves an FP context now, but a photometric
 * conversion in the measurement task is not a reason to drag double-precision
 * software emulation into a 400 ms loop. */
#define LUX_REF_EXPO_US     100800u   /* ST's reference: 63 steps exactly */
#define LUX_MATRIX_SCALE    1000000   /* the coefficients below, x1e6 */
/* Normalised counts are held x65536.
 *
 * ⚠️ NOT a taste decision, and not free at either end. It started at 1024 and
 * that number is WRONG for this application: cross-checked against ST's double
 * implementation, a reading of 12/30/9 counts at 66.6x - ordinary penumbra -
 * truncated every normalised channel to zero, so X+Y+Z came out zero and the
 * function reported "no answer" for a sample ST turns into 0.0002 lx. The
 * regime a threshold alarm lives in is exactly the one the scale was erasing.
 *
 * The ceiling is an i64 overflow in the matrix product, and it is close enough
 * to be worth writing down. Two bounds, both at the extreme corner (full-scale
 * counts, minimum gain 0x00B5, one-step exposure):
 *
 *   numerator   count x REF_US x SCALE  = 16777215 x 100800 x 65536 = 1.11e17
 *                                         (u64 holds 1.84e19)
 *   matrix acc  |Mz| row sum x norm     = 1383272 x norm, and norm peaks at
 *                                         1.11e17 / (1600 x 181) = 3.8e11
 *                                         -> 5.3e17  (i64 holds 9.22e18)
 *
 * A further factor of 16 on the scale would put the second one at 8.5e18 -
 * inside i64 by less than 10%, on a bound computed from constants that a
 * future gain table could change. 65536 keeps a factor of ~17 in hand and is
 * already 64x finer than what failed. LUX_NORM_MAX below enforces the bound
 * rather than trusting this arithmetic to stay true. */
#define LUX_NORM_SCALE      65536u
/* Hard bound on one normalised channel, from the acc bound above. The largest
 * row of |LUX_MATRIX| sums to 552625 + 335866 + 494781 = 1383272 - that sum is
 * already over all three columns, so the whole product is bounded by
 * 1383272 x LUX_NORM_MAX and NOT by three times it. At 5e12 that is 6.9e18
 * against an i64 ceiling of 9.22e18: a factor of 1.33 in hand, while the bound
 * itself sits ~13x above anything the corner case (3.8e11) can produce. So
 * reaching this clamp means the inputs were already nonsense. */
#define LUX_NORM_MAX        5000000000000LL

/* Row order X, Y, Z; column order R, G, B - the channel indices below. */
extern const i32 LUX_MATRIX[3][3];

/* Channel indices, fixed by Table 2 (see CH_NAME). Named so the matrix
 * multiply cannot silently pick up the wrong column. */
#define CH_RED      0u
#define CH_VIS      1u
#define CH_BLUE     2u
#define CH_GREEN    3u
#define CH_IR       4u
#define CH_CLEAR    5u

/* ⚠️ Cross-checked against ST's own double-precision code (VD6283_lib.c,
 * same inputs, eight cases spanning daylight, warm lamp, penumbra, a one-step
 * exposure, near saturation and a blue-heavy spectrum outside the matrix's
 * gamut). Agreement: lux within 0.001%, CCT within 0.05% wherever the reading
 * is above ~1 lx. The two remaining sources of error are both quantisation of
 * the OUTPUT, not of the maths:
 *
 *   - lux is reported in MILLILUX, so a 0.116 lx reading carries 0.9% of
 *     rounding. That is far finer than the calibration (see LUX_MATRIX) and
 *     the range - 4.29e6 lx - is what matters more for a u32;
 *   - below ~0.001 lx the millilux value reads 0 while lux_ok stays set. That
 *     is the honest answer: there IS light, and it is under the resolution of
 *     the unit it is reported in.
 *
 * The worst CCT disagreement seen was 3.4%, on a sample of a few tens of
 * counts where the reference itself is fitting noise. */
typedef struct {
    u32  lux_milli;   /* Y, in millilux (uncalibrated - see above) */
    u32  cct_k;       /* correlated colour temperature, K */
    u32  x_ten_k;     /* CIE x, x10000 */
    u32  y_ten_k;     /* CIE y, x10000 */
    bool lux_ok;      /* X+Y+Z came out positive and Y non-negative */
    bool cct_ok;      /* the cubic was evaluated inside its useful range */
} lux_photo_t;

/* Counts in, photometry out. Takes the OTP-corrected counts[], the gain in 8.8
 * and the exposure in STEPS - the same three things norm_count() takes, and for
 * the same reason.
 *
 * Returns false, with everything zeroed and both _ok flags clear, when the
 * geometry does not admit an answer: X+Y+Z at or below zero (darkness, or a
 * spectrum the matrix cannot represent) or a chromaticity on the singular line
 * y = 0.1858. Those are not errors - they are the honest answer for a sample
 * that carries no colour information, and the caller prints "--" rather than a
 * number. */
bool lux_photometric(const u32 counts[ALS_CHANNELS], u16 gain, u16 exp_steps,
                     lux_photo_t *out);

/* ── Reconstructing VIS from R+G+B ──────────────────────────────────────────
 *
 * VIS and DARK share channel 2, so only one of them can be measured at a time,
 * and switching between them per sample was tried and does not work: the
 * readings came out mixed (see vd_set_dark_mode).
 *
 * But VIS is closely predicted by the other three visible channels. Measured
 * on HW over two lighting conditions:
 *
 *     VIS = k * (RED + GREEN + BLUE),  k ~ 1.07
 *
 * and at CONSTANT light k is stable to 0.02-0.33% across samples. Between the
 * two captures it moved (1.099 -> 1.072 on A), tracking IR/VIS - so k follows
 * the SPECTRUM of the light, exactly like the autogain's clear/RGB ratio. It
 * therefore has to be measured under the light that will be used, not fixed.
 *
 * That is what "lux cal" does: with channel 2 still on VIS it averages k over
 * CAL_SAMPLES, then leaves the channel on DARK for good. From then on VIS is
 * computed and DARK is measured, so the two are SIMULTANEOUS - which is the
 * whole point for a dark-vs-vis drift measurement against temperature.
 *
 * k is held x1024. */
#define CAL_K_SCALE     1024u
#define CAL_SAMPLES     16u          /* averaged; more is slower, not better */
/* Skip budget, sized the same way KDARK_SKIP_MAX is: as many unusable samples
 * as the run needs good ones. Under the light k requires there should be none;
 * more than CAL_SAMPLES of them means the bench is wrong, not unlucky. Without
 * a bound "lux cal" in a dark box never returns - see the accumulator. */
#define CAL_SKIP_MAX    CAL_SAMPLES

/* ── Mixed VIS/DARK acquisition ("lux meas") ────────────────────────────────
 *
 * The reconstruction above buys SIMULTANEITY at the cost of never measuring
 * VIS again. This mode makes the opposite trade: channel 2 stays on the
 * visible photodiodes, so VIS is MEASURED, and the dark is sampled every
 * MEAS_DIV visible samples and held between refreshes. The corrected reading
 * is then
 *
 *     corr = VIS - (Kdark * dark_held) / K_DARK_SCALE
 *
 * ⚠️ The subtraction is done BEFORE normalisation, on raw counts. That is only
 * sound because a dark refresh is FORCED whenever gain or exposure moves (see
 * dark_forced): the held dark and the VIS it corrects therefore always come
 * from the same settings, and normalising the difference is the same as
 * normalising the two terms separately. Remove the forced refresh and this
 * order becomes wrong - it would subtract counts taken at one gain from counts
 * taken at another.
 *
 * ⚠️ A dark refresh is not one exposure window but (1 + meas_discard) of them
 * per SEL_PD write, and there are two writes - in to the covered diodes and
 * back out. Four windows at the default, ~1.6 s at 400 ms, so at MEAS_DIV=10
 * the run is about 40% slower. See MEAS_SWITCH_DISCARD.
 *
 * Kdark is held x1000, so 1000 = 1.000. The upper bound is generous on
 * purpose: the dark diodes are two, the colour channels four or five, and the
 * count is normalised over how many are selected, so the ratio that makes the
 * subtraction physical is not necessarily near unity and has to be measured. */
#define K_DARK_SCALE    1000u
#define K_DARK_MAX      100000u      /* x100.000 - a bound, not a plausible value */

/* ── Which channel the correction runs on ───────────────────────────────────
 *
 * "lux meas"     corrects VIS   (channel 2, CH_VIS)
 * "lux meas_cl"  corrects CLEAR (channel 6, CH_CLEAR)
 *
 * Everything else about the two modes is identical: same dark, same div, same
 * Kdark, same forced refresh on a settings change, same row.
 *
 * ⚠️ CLEAR IS THE WIDER DETECTOR, AND THAT IS WHY THE MODE EXISTS. Figure 17
 * counts five clear photodiodes against three visible ones, and §8.1's legend
 * says clear carries NO filter - so it collects the visible band AND the IR,
 * where VIS is IR-filtered. More diodes and more spectrum both push the same
 * way: for the same light clear returns the larger count, and the floor of a
 * threshold alarm sits proportionally further above the noise. It is also the
 * channel the autogain already steers (ag_peak_pct reads raw[CH_CLEAR]), so in
 * this mode the corrected channel and the steered channel are the same one -
 * the headroom the autogain protects is the headroom of the number produced.
 *
 * ⚠️ THE SUBTRACTION NOW CROSSES TWO CHANNELS, and Kdark is no longer the same
 * coefficient. In VIS mode dark and signal share channel 2, so the per-part
 * OTP factor is common to both terms and cancels out of the difference. In
 * CLEAR mode it does not: the dark is read on channel 2 and the signal on
 * channel 6, two channels with different photodiode counts (2 covered against
 * 5 clear), different filters and different OTP coefficients. Kdark absorbs
 * all of it - which is what it was always for, the two covered photodiodes
 * never matching the colour channels either - but the value measured for VIS
 * does NOT carry over. Re-measure it, and expect it larger: the same dark
 * current has to be scaled up against a channel that collects more light.
 *
 * ⚠️ And the two channels are clamped by DIFFERENT registers. CH1..CH5 sit
 * under DC_CLAMP_EN, which is what "lux clamp" switches; CH6 sits under
 * AC_CLAMP_EN, written to 0x01 by vd_configure() and not exposed. So "lux
 * clamp off" changes what the DARK carries and leaves CLEAR untouched, where
 * in VIS mode it moved both terms together. */
/* How far below zero the residue must sit before the row is marked as a
 * calibration fault rather than as noise. Per cent of the subtracted term.
 *
 * The per-sample scatter on these parts is 1-2.5% (measured over four
 * exposures), so 5% is about two sigma of the worst channel: noise does not
 * reach it, and a Kdark wrong by the 30% that the old shared value of 1.200
 * was on sensor A trips it on every row. */
#define MEAS_MISCAL_PCT 5u

/* ── The settings this mode needs to see anything ───────────────────────────
 *
 * ⚠️ The part REPORTS NOTHING BELOW 256 COUNTS. Par. 3.3.3 states it as the
 * "minimum reported irradiance level (guaranteed by design) is 0x000100", and
 * Table 16 repeats it as 256 counts on the 24-bit readout. Below that the
 * register reads exactly 0 - not a small number, zero.
 *
 * That is why a dark box at 1.00x / 398 ms produces a table of zeros in every
 * column, which happened twice on HW before the cause was named. Scaling this
 * bench's own measurements down from 66.67x:
 *
 *     dark  at 66.67x/400 ms  = 14753 raw  ->  222 counts at 1.00x/398 ms
 *     clear at 66.67x/1636 ms = 58078 raw  ->  212 counts at 1.00x/398 ms
 *
 * Both land under the 256 floor, so both read 0, and every quantity derived
 * from them is 0 or "--". Nothing is broken; the part is doing what it says.
 *
 * The remedy is gain, exposure, or the autogain. This threshold is the lowest
 * gain that clears the floor with a factor of about four at the default
 * exposure, from the numbers above - so it is derived from this bench, not
 * chosen. It only drives a WARNING: a bench may legitimately want low gain
 * under real light, and refusing would be wrong. */
#define ALS_MIN_REPORTED    256u    /* par. 3.3.3, counts */
#define MEAS_GAIN_IDX_WARN  7u      /* index 7 = 5.00x; above it, warn */

#define MEAS_CH_VIS     CH_VIS
#define MEAS_CH_CLEAR   CH_CLEAR

/* ── Measuring Kdark ("lux kdark cal") ──────────────────────────────────────
 *
 * In darkness CLEAR carries nothing but its own dark current, so
 *
 *     Kdark = raw_CLEAR / raw_DARK
 *
 * and both terms come out of the SAME frame: channel 2 on the covered diodes
 * carries the dark while channel 6 keeps integrating, undisturbed. No SEL_PD
 * switching, no held value, no discard - which is what makes this measurable
 * automatically where k needed the mode juggling of "lux cal".
 *
 * ⚠️ ON RAW COUNTS, and that is not a detail. meas_correct() subtracts
 * raw from raw, so Kdark must be a ratio of raws. Computing it from the
 * printed DARK and CLEAR columns instead gives a number that is wrong by the
 * ratio of the two channels' OTP coefficients - about 7% on the parts here.
 * Reading raw[] directly makes the whole question disappear.
 *
 * ⚠️ IT MUST BE RUN IN DARKNESS, and no code here can guarantee that. Any
 * light lands on CLEAR and not on the covered diodes, so it inflates the
 * ratio without limit - a Kdark measured under a lit bench is a measurement of
 * the light, not of the part. The two checks below catch the gross case only:
 *
 *   - a ratio above KDARK_RATIO_MAX is refused outright. Measured 1.06 and
 *     1.33 on the two parts of this bench (2026-09-02, 66.67x, four exposures
 *     from 200 to 1600 ms, ratio constant to 1%), so 5.000 sits about 4x above
 *     anything the silicon has shown and only light can reach it;
 *   - the per-sample spread is reported. A steady dark gives a tight one; a
 *     wandering one means something was changing, and light is the usual
 *     something.
 *
 * Neither test can tell darkness from DIM light: at a ratio of 3 the command
 * accepts a value that is mostly illumination. The guarantee is procedural -
 * a closed box - and the numbers are there to be looked at, not to excuse not
 * looking. */
#define KDARK_SAMPLES    16u    /* averaged, as CAL_SAMPLES is for k */
#define KDARK_RATIO_MAX  5000u  /* x1000: above this it is light, not dark */
/* Skip budget: as many bad samples as good ones, and then give up.
 *
 * ⚠️ Sized against the CAUSE, not against patience. The measurement runs at
 * maximum gain and 1636 ms, so in darkness there should be ZERO skips - one
 * would already be surprising. More of them than the run needs good samples
 * means the premise is wrong (channel 2 not on the covered diodes, or the box
 * open), not that the bench was unlucky. It was 64, which at 1.64 s a sample
 * is nearly two minutes of a console saying nothing before admitting it. */
#define KDARK_SKIP_MAX   KDARK_SAMPLES
#define MEAS_DIV_MIN    1u           /* a dark before every visible sample */
#define MEAS_DIV_MAX    255u

/* Frames thrown away after each SEL_PD write, before one is trusted.
 *
 * ⚠️ 0 DOES NOT WORK. Measured on HW 2026-08-31, fixed 1.00x/400 ms, Kdark=0,
 * a dark before every sample: the visible reading came back as EXACTLY 0 on
 * every row of sensor B and on alternate rows of sensor A, and the dark went
 * 545, 545, 545, 866, 7076, 11639, 21248, 28517 - partial frames, not light.
 * A count of exactly zero is the signature: the registers had not been
 * refreshed when they were read.
 *
 * The datasheet argument said 0 should be enough, and it was wrong. Par. 3.3
 * has the part finish measurement N and STOP, waiting for the host handshake
 * before starting N+1 - so SEL_PD is written while nothing is integrating, and
 * the next frame should start clean after the switch. It does not, and the
 * reason is not established: either the handshake does not idle the part the
 * way that reads, or vd_next_frame() returns before the frame is truly
 * complete (par. 4.3.3 describes INTR_ST=1 for both "no interrupt triggered"
 * and "last interrupt not cleared", which cannot both be one bit, so the
 * polling condition rests on a sentence that does not parse).
 *
 * Keep it at 1 unless a measurement says otherwise. This is the exact spot
 * where the previous attempt at a per-sample dark produced a "dark" that
 * followed the light.
 *
 * Settable at runtime as the optional third argument of "lux meas", so a value
 * can be swept without a rebuild; this is only the default.
 *
 * ⚠️ Do NOT reach for this knob to explain a dark that follows the light. That
 * was measured too, and it is not a timing artefact: at 1.00x/400 ms the held
 * dark went 266 -> 1468 (sensor A) and 292 -> 2037 (B) over a 145x change in
 * the light, and the STATIC dark mode gave 1590 and 1992 under the same light -
 * the same values. The switching is not what makes the dark track the light;
 * the covered photodiodes really do see about 0.06-0.08% of the visible. See
 * the note on the leak in ca_os_main_app_50.c. */
#define MEAS_SWITCH_DISCARD  1u
#define MEAS_DISCARD_MAX     8u

/* Autogain: the clear-to-RGB ratio, x256. See ag_update() for why it is
 * learned at runtime rather than fixed - measured 8.4..24.6 on HW, following
 * the spectrum of the light. AG_K_TYPICAL only seeds the very first sample. */
#define AG_HIGH_PCT     85u     /* above this, back off */
#define AG_LOW_PCT      40u     /* below this, push on  */
#define AG_TARGET_PCT   70u     /* where a correction aims */
#define AG_SETTLE       1u      /* samples to bin after a change */

#define AG_K_SCALE          256u
#define AG_K_TYPICAL        (18u * AG_K_SCALE)

#define Q_DEPTH             8u    /* >= 8 as required; with two sensors at
                                   * ~1.6 s per sample that is ~6.5 s of slack */
#define SENSOR_COUNT        2u

/* How often an idle measurement task looks at its run flag. Short enough that
 * "lux start" feels immediate, long enough to cost nothing. */
#define IDLE_POLL_MS        50u

/* Frame completion is polled on INTR_ST rather than assumed from a fixed
 * delay. The poll starts only after one full exposure has elapsed, so these
 * are just the fine steps at the end: short enough not to add jitter, long
 * enough not to hammer the bus. The timeout is generous - it exists to keep a
 * dead sensor from hanging the task, not to bound normal operation. */
#define FRAME_POLL_MS       5u
/* Derived from the sensor's CURRENT exposure and idle gap, both runtime values:
 * at 1636 ms a timeout fixed to the 400 ms default would fire every frame, and
 * so would one that ignored a 5 s ALS_PERIOD. */
#define FRAME_TIMEOUT_MS(s) (exposure_ms(s) + IM_STEPS_TO_MS((s)->im_steps) + 500u)

/* Exposure limits, from set_exposure(): steps of 1600 us, clamped to
 * [1, 0x3FF] - i.e. 1.6 ms to 1636.8 ms. */
#define EXTIME_STEPS_MIN    1u
#define EXTIME_STEPS_MAX    0x3FFu


/* A full sample is 7 values: the 6 ALS channels plus the dark reference.
 *
 * ONE exposure window per sample, since the dark stopped being sampled: all
 * six channels come out of a single frame, and channel 2 carries either the
 * visible or the dark depending on the mode (see vd_set_dark_mode). The old
 * cycle cost FOUR windows because it switched channel 2 in and out on every
 * sample, discarding the two frames the switches landed in - and it produced a
 * dark that followed the light anyway.
 *
 * ~404 ms per sample at the default exposure and no idle gap. Nominal: the
 * task does not sleep for it but waits on INTR_ST, so the real pace follows
 * the sensor's own clock. */
#define SAMPLE_PERIOD_MS(s) (exposure_ms(s) + IM_STEPS_TO_MS((s)->im_steps) \
                             + CONV_TIME_MS)

/* Exposure in milliseconds for a given step count, so the two can never drift
 * apart. 249 -> 398 ms, 0x3FF -> 1636 ms. See EXTIME_PLUS_ONE for why there is
 * no longer a +1 here. */
#define STEPS_TO_MS(st)     ((EXTIME_UNITS(st) * EXTIME_US_PER_STEP) / 1000u)
/* The same limits in milliseconds, for the menu that asks in milliseconds.
 * Derived from the step limits rather than written out, so the convention of
 * EXTIME_PLUS_ONE can only ever change them both at once: the prompt used to
 * say "3..1638" as literals, which was correct for the +1 reading and wrong
 * the moment it was dropped. */
#define EXTIME_MS_MIN       STEPS_TO_MS(EXTIME_STEPS_MIN)
#define EXTIME_MS_MAX       STEPS_TO_MS(EXTIME_STEPS_MAX)

/* One sample: all six channels from one sensor at one instant. */
typedef struct {
    u32 counts[ALS_CHANNELS];   /* per-part calibrated counts (OTP applied) */
    u32 raw[ALS_CHANNELS];      /* the same values before calibration */
    u32 dark;                   /* covered-photodiode count, 0 if not measured */
    u32 seq;                    /* per-sensor sample number */
    u32 dropped;                /* samples the queue has lost so far */
    u32 vis_calc;               /* VIS reconstructed from R+G+B, 0 if not */
    u32 cal_k;                  /* the coefficient used, x1024, 0 if none */
    u32 sat;                    /* saturation level for this exposure (raw) */

    /* ── Mixed acquisition ("lux meas"). Computed in the measurement task and
     * carried HERE rather than read back from s_sensor[] by the printing task:
     * that table belongs to the measurer, and the autogain moves it between
     * the moment a sample is queued and the moment it is printed. */
    /* ⚠️ Named for the ROLE, not for the channel: "lux meas_cl" puts CLEAR
     * here where "lux meas" puts VIS, and a field called vis_corr holding a
     * clear reading is the kind of name that survives into a capture and lies
     * about it months later. meas_ch below says which channel it was. */
    /* ── Photometry, computed per sample (see lux_photometric) ───────────
     * In MILLILUX, matching the KDARK column's habit of carrying a x1000
     * integer rather than inventing a decimal formatter for one field.
     *
     * ⚠️ Independent of the correction above and of the mode: it is built
     * from R, G and B, so it stands even when sig_corr does not - a pinned
     * signal channel blanks NCLR and leaves LUX perfectly good. The reverse
     * also happens: lux_ok goes clear when R, G or B is pinned while the
     * signal channel still has headroom. */
    u32 lux_milli;
    u8  lux_ok;                 /* 1 if lux_milli is a measurement */

    /* ⚠️ SIGNED, and no longer clamped at zero. It was, and that was right
     * while Kdark was a guess: a negative result then meant the coefficient
     * was too high, so zero plus a "!" was the honest report.
     *
     * Once "lux kdark cal" measures Kdark, the correction is calibrated to
     * make this quantity ZERO in darkness - so at the floor it is noise about
     * zero and HALF OF IT IS NEGATIVE by construction. Clamping threw that
     * half away and marked every row, which is exactly the wrong way round:
     * measured on HW 2026-09-02, all 22 rows of a two-sensor dark run clamped
     * (p = 2.4e-7 if the residue were centred noise), so the marker stopped
     * distinguishing anything and the noise below zero - the very quantity a
     * threshold has to clear - became unmeasurable.
     *
     * The negative half is not an error to hide. It is the floor. */
    i32 sig_corr;               /* signal - Kdark*dark, raw counts, SIGNED */
    i32 nsig_corr;              /* the same, normalised to NORM_GAIN/NORM_STEPS */
    u8  meas_ch;                /* MEAS_CH_VIS or MEAS_CH_CLEAR; the channel
                                 * sig_corr was computed on. Carried in the
                                 * sample rather than read back from s_sensor[]
                                 * for the same reason as everything else here:
                                 * the CLI can re-arm the mode between the
                                 * moment a row is queued and the moment it is
                                 * printed, and the row must describe itself. */
    u32 dark_used;              /* the held dark this row was corrected with */
    u32 k_dark;                 /* the coefficient used, x1000 */
    u16 gain;                   /* gain value in force, 8.8 (0x0100 = 1x) */
    u16 exp_ms;                 /* exposure that produced it, for display */
    u16 exp_steps;              /* EXTIME that produced it - what norm_count
                                 * scales on, because the millisecond value is
                                 * a truncated integer (113.6 -> 113). */
    u8  bus;                    /* I2C instance it came from */
    u8  sensor;                 /* 0 = A, 1 = B */
    u8  saturated;              /* bitmask: bit N set if channel N clipped */
    u8  settling;               /* 1 if taken while the autogain change from the
                                 * previous sample was still working through:
                                 * the frame straddles two settings and the row
                                 * must be excluded from any fit */
    u8  dark_mode;              /* 1 = channel 2 carried DARK, not visible */
    u8  otp_on;                 /* 1 if counts[] carry the factory correction */
    u8  adjusted;               /* 1 if autogain changed gain/exposure after it */
    u8  calibrating;            /* 1 if taken while measuring k (see lux cal) */
    u8  meas;                   /* 1 if this row belongs to a mixed acquisition */
    u8  dark_valid;             /* 1 if dark_used holds a measured dark. Distinct
                                 * from corr_ok: a pinned VIS invalidates the
                                 * correction while leaving the dark perfectly
                                 * good, and that row must still show it */
    u8  corr_ok;                /* 1 if sig_corr/nsig_corr hold a real result.
                                 * Its own flag rather than "sig_corr != 0":
                                 * zero is a LEGITIMATE corrected value once the
                                 * subtraction clamps, and conflating the two
                                 * would print "--" for the darkest rows - the
                                 * ones a threshold alarm cares about most */
    u8  dark_fresh;             /* 1 if the dark was re-read just before it */
    /* ⚠️ Re-purposed, not merely renamed. It used to mean "the subtraction hit
     * the clamp", which after the change above happens on half the rows of any
     * dark run and says nothing. It now means the residue is further below zero
     * than MEAS_MISCAL_PCT of what was subtracted - i.e. Kdark is genuinely too
     * high for this part, which is a fault worth a marker because it is rare
     * again. Ordinary negative noise no longer raises it. */
    u8  miscal;                 /* 1 if Kdark looks out of calibration */
} lux_sample_t;

/* Channel order is fixed by the part: DS13735 Table 2 "channel affectation".
 * CH1..CH6 = red, visible, blue, green, IR, clear - so the register at
 * 0x06+4*ch and this table line up by construction.
 *
 * There is no seventh entry: the DARK photodiodes have no data register of
 * their own. Table 3 calls them "1 internal dark channel" and Figure 1 draws
 * them with an amplifier but no ADC. They are read through channel 2 instead:
 * SEL_PD_x(1) selects either the three visible photodiodes (0x07) or the two
 * covered ones (0x18), so dark and visible share one ADC and cannot be sampled
 * at the same time. That is why VIS is reconstructed as k*(R+G+B).
 *
 * The dark count is NOT subtracted from the colour channels, here or anywhere
 * else: the ST driver only ever multiplies (apply_calfactor is k*raw, no
 * additive term), and the datasheet never claims the part compensates its own
 * dark current - Figure 1 shows a connection, which is not an operation.
 * Leaving it in is deliberate and correct for this application: dark(T) is the
 * quantity being measured, not an error to remove, and a 1:1 subtraction would
 * be wrong anyway because the two covered photodiodes do not match the 4-5
 * photodiodes of each colour channel in area, filter or doping.
 *
 * clear and visible are not redundant: clear has NO filter (visible + IR),
 * visible is IR-filtered. Their difference is the IR content of the light. */
extern const char *const CH_NAME[ALS_CHANNELS];


/* ============================================================================
 * SHARED STATE
 * ========================================================================== */

/* Union-based definition: the macro sizes the storage and pre-initialises the
 * header, and access goes through .q so the type pun stays legal. Never cast
 * the raw buffer to ca_queue_static_t* — that is strict-aliasing UB, and this
 * project has already been bitten by it once in a -O3 Release build. */

/* Why a sensor is not delivering. "No answer" and "wrong part" look identical
 * from the outside but need opposite investigations: the first is wiring or
 * bus, the second is the address or the device itself. */
typedef enum {
    FAIL_NONE = 0,
    FAIL_NO_ACK,        /* the transfer failed: nobody drove the bus */
    FAIL_BAD_ID,        /* something answered, but DEVICE_ID is wrong */
    FAIL_CONFIG,        /* identified, then a configuration write failed */
} vd_fail_t;

/* The part's unique id, as ST builds it: 14 characters plus the terminator,
 * derived from OTP bits 60..112 (otp_generate_uid in the ST driver). */
#define VD_UID_LEN      16u

/* Everything the OTP yields for one part. Valid only when `read` is true;
 * until then the defaults below stand in and every reading is nominal. */
typedef struct {
    bool read;                      /* the OTP was successfully decoded */
    u8   gains[ALS_CHANNELS];       /* raw per-channel coefficient bytes */
    u16  hf_trim;                   /* 9 bits, 10.24 MHz oscillator */
    u8   lf_trim;                   /* 4 bits, 48.7 kHz oscillator */
    u8   version;
    /* ⚠️ Identifies the PART, not the bus. "A" and "B" are bus positions: the
     * two sensors were found swapped between one session and the next, and
     * their dark floors differ by 12%, so a per-part thermal calibration
     * attached to a label rather than to this string will eventually be
     * applied to the wrong die. Printed in every capture for that reason. */
    char uid[VD_UID_LEN];
} vd_otp_t;

typedef struct {
    u8           bus;           /* I2C instance number */
    u8           alt;           /* pin map selector */
    const char  *label;         /* "A" / "B" */
    const char  *pins;          /* human-readable pin text */
    bool         up;            /* ca_i2c_init() succeeded */
    bool         present;       /* DEVICE_ID read back correctly */
    volatile bool running;      /* cleared by "lux stop", read by the task */
    u8           dev_id;
    u8           rev_id;
    vd_fail_t    fail;          /* why the last bring-up attempt failed */

    /* ── Per-part calibration and acquisition parameters ─────────────────
     * Written by the CLI only while the task is parked (see "lux setup"),
     * so no lock is needed: a running sensor is never reconfigured from
     * another thread. The autogain moves gain_idx/exp_steps from INSIDE
     * the measurement task, which is the only writer while running. */
    vd_otp_t     otp;
    bool         otp_on;        /* apply the factory correction */
    /* DC clamp on channel 2. True is the ST behaviour and the default; false
     * leaves the dark current in the signal. Read by vd_configure() only, so it
     * takes effect at the next start and never races the measurement task. */
    bool         ch2_clamp;
    bool         auto_gain;     /* autogain enabled */
    u16          exp_steps;     /* EXTIME, 1..0x3FF */
    u8           im_steps;      /* ALS_PERIOD, 0..255 (see IM_STEP_MS) */
    u8           gain_idx;      /* index into GAIN_VALUE, 0 = highest gain */
    u8           settle;        /* samples to discard after a parameter change */
    volatile u32 adjusts;       /* times the autogain moved gain or exposure */
    u32          ag_k;          /* clear/mean(RGB) ratio x256, learned at runtime */

    /* VIS reconstruction (see CAL_K_SCALE). cal_k is valid only once cal_done
     * is set; until then VIS is whatever channel 2 actually holds. */
    u32          cal_k;         /* VIS/(R+G+B) x1024 */
    bool         cal_done;
    volatile u8  cal_req;       /* samples still to average, 0 = not calibrating */
    bool         cal_dark_was;  /* dark mode to restore when cal finishes */
    volatile bool cal_stop;     /* cal started the sensor: stop it when done */
    u32          cal_acc;       /* running sum of per-sample k */
    u32          cal_n;         /* how many went into cal_acc */
    u32          cal_min, cal_max;   /* to report the spread */
    u32          cal_sat;        /* samples skipped because a channel was pinned */
    volatile u32 woke;          /* times the task started a bring-up */
    volatile u32 frozen;        /* consecutive reads identical to the previous */
    u32          last_clear;    /* previous CH6, to spot a stale frame */
    volatile bool dark_mode;    /* channel 2 is on the covered photodiodes */
    volatile bool dark_req;     /* requested mode, applied by the task */
    volatile u8   probe_req;    /* 0 = none, else the channel to sweep + 1 */
    volatile bool probe_stop;   /* the sweep started the sensor: park it after */
    volatile bool bytes_req;    /* dump the raw channel bytes (diagnostic) */
    volatile bool bytes_stop;   /* the dump started the sensor: park it after */
    volatile u32 last_dark;     /* most recent dark reading */

    /* ── Mixed acquisition (see K_DARK_SCALE) ────────────────────────────
     * Only the measurement task writes dark_hold/vis_since_dark; the CLI
     * writes meas_on, meas_div and k_dark while the task is between frames,
     * and sets dark_forced. */
    volatile bool meas_on;      /* mixed VIS/DARK acquisition in force */
    /* Which channel the correction is computed on: MEAS_CH_VIS for "lux meas",
     * MEAS_CH_CLEAR for "lux meas_cl". The dark comes from channel 2 either
     * way - it is the only channel that can carry it - so in CLEAR mode the
     * subtraction crosses two channels and Kdark absorbs the mismatch. */
    u8           meas_ch;
    u16          meas_div;      /* visible samples between two dark reads */
    u8           meas_discard;  /* frames binned after each SEL_PD write; an
                                 * open experiment, see MEAS_SWITCH_DISCARD */
    /* ⚠️ PER SENSOR, and it always was in this struct - it was only the CLI
     * that forced one value onto both. It absorbs the ratio of that part's own
     * CH2 and CH6 OTP coefficients, so it is a per-part calibration exactly
     * like cal_k: measured 1.06 on A against 1.33 on B, which no single shared
     * number can serve (1.200 over-subtracted on A until every row clamped to
     * zero, and under-subtracted on B). */
    u32          k_dark;        /* dark correction coefficient, x1000 */

    /* ── Measuring it (see KDARK_SAMPLES) ────────────────────────────────
     * Same shape as the cal_* block above, and for the same reasons: the CLI
     * only sets kd_req, and the task that owns the bus does the work. */
    volatile u8  kd_req;        /* samples still to average, 0 = not measuring */
    bool         kd_dark_was;   /* dark mode to restore when it finishes */
    /* ⚠️ The measurement sets its OWN gain and exposure, to the maximum, and
     * puts these back afterwards. It is not a preference: at 1.00x/398 ms in a
     * closed box every channel reads exactly 0 - CLEAR included - so the ratio
     * has no numerator and no denominator. Measured on HW 2026-09-02.
     *
     * Maximum, not "enough": there is nothing to search for. The wanted signal
     * IS the dark current, so more gain and more integration is always better
     * until something clips, and in darkness nothing does.
     *
     * It also buys a darkness check for free. At full sensitivity ordinary room
     * light puts CLEAR around 20% of full scale while the covered diodes barely
     * move, so the ratio lands near 50 and KDARK_RATIO_MAX refuses it. Running
     * this command with the lid off fails loudly instead of quietly returning a
     * measurement of the room. */
    u8           kd_gain_was;   /* gain index to restore */
    u16          kd_exp_was;    /* EXTIME to restore */
    bool         kd_auto_was;   /* autogain state to restore */
    volatile bool kd_stop;      /* it started the sensor: park it again after */
    u64          kd_clear;      /* running sum of raw CLEAR */
    u64          kd_dark;       /* running sum of raw DARK */
    u32          kd_n;          /* how many went into the two sums */
    u32          kd_min, kd_max;/* per-sample ratio x1000, to report the spread */
    u32          kd_skip;       /* samples skipped: pinned, or a dark of zero */
    u32          dark_hold;     /* the dark in force, raw counts */
    bool         dark_hold_ok;  /* a dark has been measured at least once */
    u16          vis_since_dark;/* visible samples since the last dark read */
    /* Set whenever gain or exposure moves - by the autogain from inside the
     * task, or by the menu from the CLI. The held dark was taken at the OLD
     * settings, and the subtraction happens before normalisation, so carrying
     * it across the change would subtract counts from a different scale. */
    volatile bool dark_forced;
    volatile bool dark_was_fresh;   /* the refresh above happened: mark the row */

    volatile u32 samples;       /* samples produced */
    volatile u32 errors;        /* failed I2C transactions */
    volatile u32 dropped;       /* samples the queue could not take */
} sensor_ctx_t;

/* Designated initialisers: the rest (counters, flags) is zeroed by
 * the C runtime and filled in by ca_os_configure. */






/* ============================================================================
 * PURE COMPUTATION — no I2C, no hardware state
 * ========================================================================== */

/* Saturation level for the exposure currently in force (see ALS_SAT_*). Takes
 * the STEP count, not milliseconds: the datasheet's threshold is on the step. */
u32  als_sat_level(u16 exp_steps);

/* Exposure in ms actually in force on one sensor. */
u32  exposure_ms(const sensor_ctx_t *s);

/* OTP bit unpacking. otp_field() reads one field out of the two 64-bit banks
 * loaded by the I2C side; `swap` reverses the bit order within the field. */
u8   otp_bit_reverse(u8 d);
u32  otp_field(const u64 bits[2], u32 bit_start, u32 bit_nb, bool swap);
void otp_defaults(vd_otp_t *o);

/* Build the part's unique id into o->uid, exactly as ST's otp_generate_uid()
 * does: a running XOR over the nibbles of OTP bits 60..112, rendered as hex
 * characters. Pure arithmetic on bits the I2C side has already loaded. */
void otp_uid(vd_otp_t *o, const u64 bits[2]);

/* The per-channel correction: coefficient, then its application to a count. */
u8   otp_calfactor(u8 otp_gain, u16 channel_gain);
u32  otp_apply(u32 raw, u8 factor);

/* Autogain. ag_update() decides and writes the new settings into the context;
 * applying them to the part is the I2C side's job (ag_apply). */
u32  ag_peak_pct(const lux_sample_t *smp);
u32  ag_rgb_mean(const lux_sample_t *smp);
bool ag_solve(u32 est, u8 gi, u16 st, u8 *out_gi, u16 *out_st);
bool ag_update(sensor_ctx_t *s, const lux_sample_t *smp);

/* Gain as N.MM without floating point (ca_vsnprintf has no %f). */
void gain_text(u8 idx, u32 *whole, u32 *frac);

/* Scale a count to the reference gain and exposure (see NORM_GAIN).
 *
 * ⚠️ Takes the STEP count, not milliseconds. The millisecond value is a
 * truncated integer - STEPS_TO_MS(70) is 113.6 rendered as 113 - so dividing
 * by it carried half a percent at short exposures. Steps are exact, and the
 * ratio (steps+1)/(ref+1) is the same under either reading of §4.3.12. */
u32  norm_count(u32 v, u16 gain, u16 exp_steps);

/* The mixed-acquisition correction: the signal channel minus the scaled dark,
 * on RAW counts and before normalisation (see K_DARK_SCALE for why that order
 * is sound).
 *
 * ⚠️ Returns a SIGNED residue and does NOT clamp. Going below zero is the
 * normal state at the floor once Kdark is measured rather than guessed - see
 * sig_corr. What must never happen is the u32 WRAP that clamping originally
 * existed to prevent: a dark larger than the signal came out as a number near
 * 4e9 and printed as an enormous light level. Signed arithmetic removes the
 * wrap without removing the information.
 *
 * `*miscal` is raised only when the residue sits more than MEAS_MISCAL_PCT
 * below zero relative to what was subtracted - that is a Kdark too high for
 * this part, not the ordinary negative half of the noise. */
i32  meas_correct(u32 sig, u32 dark, u32 k_dark, bool *miscal);

/* The sensor table, defined in vd6283_ctrl.c and shared by every file. */
extern sensor_ctx_t s_sensor[SENSOR_COUNT];

#endif /* EN_I2C */
#endif /* VD6283_CTRL_H */

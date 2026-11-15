*** Comments ***
caOS on Renode, beyond the boot: peripherals and features the simulation models
for caOS (simu/CHANGES.md 26-29). Run: simu/test.sh
  - ex29: SPI1 master -> SPI3 slave through spi_pair_f4.py, jumpers from simu/wires
  - reset: system reset from the CLI, the board must boot again (InitVectorTableOffset)
  - reboot: the IWDG armed and left unfed must restart the board
  - ex35: Ethernet through the host TAP caos-tap0 (run.sh -e): DHCP lease, then a
    ping from the PC. Skipped unless the TAP is ready (sudo tools/bench_net.sh
    tap up): it takes the bench subnet, so it never runs together with the bench.
Each test is tagged with its board; a test whose ELF is not built is skipped.

*** Settings ***
Suite Setup       Renode Suite Setup
Suite Teardown    Renode Suite Teardown
Test Teardown     Renode Test Teardown
Resource          ${RENODEKEYWORDS}
Resource          caos.resource

*** Keywords ***
SPI Master Slave Should Work
    [Arguments]    ${board}
    Boot Example    ${board}    29    wired=${True}
    # the slave's own line: the master prints "IRQ M: frame" even with no slave
    Wait For Line On Uart    EX29 IRQ: frame 3 OK
    Should Not Be On Uart    ERR    timeout=2
    Write Line To Uart    wire
    Wait For Line On Uart    WIRE RESULT : OK${SPACE}${SPACE}(3/3)
    # wire leaves the pins as GPIO and reboots: the board must come back
    Wait For Line On Uart    -- c.a.O.S --
    Wait For Line On Uart    EX29 IRQ: frame 1 OK

Reset Should Restart
    [Arguments]    ${board}
    Boot Example    ${board}    2
    Wait For Line On Uart    example_2: scheduler demo, idle
    Write Line To Uart    reset
    Wait For Line On Uart    reset: system reset
    Wait For Line On Uart    -- c.a.O.S --
    Wait For Line On Uart    example_2: scheduler demo, idle

Watchdog Reboot Should Restart
    [Arguments]    ${board}
    Boot Example    ${board}    10
    # ex10 prints its title BEFORE the banner: wait for the end of the banner
    Wait For Line On Uart    Baudrate: 115200 bps
    Write Line To Uart    reboot
    Wait For Line On Uart    reboot: arming the watchdog, feed stopped
    Wait For Line On Uart    -- c.a.O.S --    timeout=20
    Wait For Line On Uart    Baudrate: 115200 bps

Ethernet DHCP And Ping Should Work
    [Arguments]    ${board}
    ${addr}=    Run Process    ip    -o    -4    addr    show    dev    ${TAP}
    ${ready}=    Evaluate    ' 192.168.10.1/24 ' in $addr.stdout
    Skip If    not ${ready}    ${TAP} not ready (sudo tools/bench_net.sh tap up)
    Boot Example    ${board}    35    eth=${True}
    # dnsmasq pings the address before offering it: ~3 s of host time
    ${r}=    Wait For Line On Uart    DHCP BOUND    timeout=60
    ${ip}=    Evaluate    re.search(r'IP=([0-9.]+)', str($r)).group(1)    re
    Start Emulation
    ${p}=    Run Process    ping    -c    3    -W    3    ${ip}
    Log    ${p.stdout}
    Should Be Equal As Integers    ${p.rc}    0    no ping reply from ${ip}

*** Test Cases ***
F767 Example 29 SPI Master Slave
    [Tags]    f767
    SPI Master Slave Should Work    f767

F439 Example 29 SPI Master Slave
    [Tags]    f439
    SPI Master Slave Should Work    f439

H753 Reset Restarts
    [Tags]    h753
    Reset Should Restart    h753

F767 Reset Restarts
    [Tags]    f767
    Reset Should Restart    f767

F439 Reset Restarts
    [Tags]    f439
    Reset Should Restart    f439

F446 Reset Restarts
    [Tags]    f446
    Reset Should Restart    f446

U575 Reset Restarts
    [Tags]    u575
    Reset Should Restart    u575

H563 Reset Restarts
    [Tags]    h563
    Reset Should Restart    h563

H773 Reset Restarts
    [Tags]    h773
    Reset Should Restart    h773

H753 Watchdog Reboot Restarts
    [Tags]    h753
    Watchdog Reboot Should Restart    h753

F767 Watchdog Reboot Restarts
    [Tags]    f767
    Watchdog Reboot Should Restart    f767

F439 Watchdog Reboot Restarts
    [Tags]    f439
    Watchdog Reboot Should Restart    f439

F446 Watchdog Reboot Restarts
    [Tags]    f446
    Watchdog Reboot Should Restart    f446

U575 Watchdog Reboot Restarts
    [Tags]    u575
    Watchdog Reboot Should Restart    u575

H563 Watchdog Reboot Restarts
    [Tags]    h563
    Watchdog Reboot Should Restart    h563

H773 Watchdog Reboot Restarts
    [Tags]    h773
    Watchdog Reboot Should Restart    h773

F767 Example 35 Ethernet DHCP Ping
    [Tags]    f767    eth
    Ethernet DHCP And Ping Should Work    f767

F439 Example 35 Ethernet DHCP Ping
    [Tags]    f439    eth
    Ethernet DHCP And Ping Should Work    f439

H753 Example 35 Ethernet DHCP Ping
    [Tags]    h753    eth
    Ethernet DHCP And Ping Should Work    h753

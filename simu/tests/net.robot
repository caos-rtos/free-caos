*** Comments ***
Network regression without boards: the real firmware on Renode, its MAC wired to
the host TAP caos-tap0 (run.sh -e), the PC as peer.

  - ex35 storm: DHCP lease, ping, then a storm of malformed frames from the PC
    (simu/net/inject.py through `sudo tools/bench_net.sh inject`: targeted
    breakage of every parser rule plus seeded scapy fuzz()). Afterwards the
    board must still answer ping and its CLI, and must not have printed a fault
    or a safeguard. The PC fuzzer (caos/net/test/fuzz) covers the pure parsers;
    this covers what they feed - driver, buffer pool, net task, DHCP/ARP/UDP
    services - on the code that actually runs on the MCU.

Skipped unless the TAP is ready (sudo tools/bench_net.sh tap up): it takes the
bench subnet, so it never runs together with the bench. ETH boards only.
STORM_COUNT / STORM_SEED override the random part (default 2000 frames, seed 1).

*** Settings ***
Suite Setup       Renode Suite Setup
Suite Teardown    Renode Suite Teardown
Test Teardown     Renode Test Teardown
Resource          ${RENODEKEYWORDS}
Resource          caos.resource

*** Variables ***
${STORM_COUNT}    2000
${STORM_SEED}     1
# the banners of caos/src/os_handlers.c and safeguard.c
${FAULT_RE}       \\[(BUS FAULT|MPU FAULT|USAGE FAULT|SAFEGUARD)

*** Keywords ***
Tap Should Be Ready
    ${addr}=    Run Process    ip    -o    -4    addr    show    dev    ${TAP}
    ${ready}=    Evaluate    ' 192.168.10.1/24 ' in $addr.stdout
    Skip If    not ${ready}    ${TAP} not ready (sudo tools/bench_net.sh tap up)

Board Should Answer Ping
    [Arguments]    ${ip}
    ${p}=    Run Process    ping    -c    3    -W    3    ${ip}
    Log    ${p.stdout}
    Should Be Equal As Integers    ${p.rc}    0    no ping reply from ${ip}

TCP Echo Should Work
    [Arguments]    ${ip}
    ${r}=    Run Process    /usr/bin/python3    ${ROOT}/simu/net/tcp_echo.py    ${ip}
    ...    timeout=20s
    Log    ${r.stdout}\n${r.stderr}
    Should Contain    ${r.stdout}    caOS-echo    TCP echo on ${ip}:7 failed: ${r.stderr}

Board Should Survive A Storm
    [Arguments]    ${board}    ${ex}=35    ${echo}=${False}
    Tap Should Be Ready
    Boot Example    ${board}    ${ex}    eth=${True}
    ${r}=    Wait For Line On Uart    DHCP BOUND    timeout=60
    ${ip}=    Evaluate    re.search(r'IP=([0-9.]+)', str($r)).group(1)    re
    Start Emulation
    Board Should Answer Ping    ${ip}
    IF    ${echo}
        TCP Echo Should Work    ${ip}
    END

    ${bench}=    Normalize Path    ${ROOT}/tools/bench_net.sh
    ${s}=    Run Process    sudo    -n    ${bench}    inject    ${ip}
    ...      ${STORM_COUNT}    ${STORM_SEED}    timeout=900s
    Log    ${s.stdout}\n${s.stderr}
    Should Be Equal As Integers    ${s.rc}    0    the storm could not be sent: ${s.stderr}

    # Let the net task drain what is queued, then the board must be whole
    Sleep    3s
    Board Should Answer Ping    ${ip}
    IF    ${echo}
        TCP Echo Should Work    ${ip}
    END
    Write Line To Uart    arp info
    Wait For Line On Uart    ARP cache:    timeout=20
    Start Emulation
    Should Not Be On Uart    ${FAULT_RE}    timeout=2    treatAsRegex=true

*** Test Cases ***
F767 Example 35 Survives A Malformed Frame Storm
    [Tags]    f767    eth    storm
    Board Should Survive A Storm    f767

H753 Example 35 Survives A Malformed Frame Storm
    [Tags]    h753    eth    storm
    Board Should Survive A Storm    h753

F439 Example 35 Survives A Malformed Frame Storm
    [Tags]    f439    eth    storm
    Board Should Survive A Storm    f439

F767 Example 41 TCP Server Survives A Malformed Frame Storm
    [Tags]    f767    eth    storm    tcp
    Board Should Survive A Storm    f767    ex=41    echo=${True}

H753 Example 41 TCP Server Survives A Malformed Frame Storm
    [Tags]    h753    eth    storm    tcp
    Board Should Survive A Storm    h753    ex=41    echo=${True}

F439 Example 41 TCP Server Survives A Malformed Frame Storm
    [Tags]    f439    eth    storm    tcp
    Board Should Survive A Storm    f439    ex=41    echo=${True}

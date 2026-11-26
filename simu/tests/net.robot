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

Board Should Fragment Over A Reduced MTU
    [Arguments]    ${board}
    [Documentation]    ex48 (UDP echo 7961, EN_IP_FRAG): at every MTU of the radio
    ...    range the echoes over it leave the board as fragments that the PC
    ...    reassembles byte-exact; a big ping reply too. ip_frag_mtu.md sec. 4.1
    Tap Should Be Ready
    Boot Example    ${board}    48    eth=${True}
    ${r}=    Wait For Line On Uart    DHCP BOUND    timeout=60
    ${ip}=    Evaluate    re.search(r'IP=([0-9.]+)', str($r)).group(1)    re
    Start Emulation
    Board Should Answer Ping    ${ip}
    FOR    ${mtu}    IN    512    576    1006    1280    1480
        Write Line To Uart    ip mtu ${mtu}
        Wait For Line On Uart    MTU set to ${mtu}    timeout=20
        Start Emulation
        ${u}=    Run Process    /usr/bin/python3    ${ROOT}/simu/net/frag_check.py
        ...      udp    ${ip}    7961    100    1000    1472    1800    timeout=90s
        Log    ${u.stdout}\n${u.stderr}
        Should Be Equal As Integers    ${u.rc}    0    UDP echo at MTU ${mtu}: ${u.stdout}${u.stderr}
    END
    Write Line To Uart    ip mtu 512
    Wait For Line On Uart    MTU set to 512    timeout=20
    Start Emulation
    ${p}=    Run Process    ping    -c    3    -W    5    -s    1400    ${ip}
    Log    ${p.stdout}
    Should Be Equal As Integers    ${p.rc}    0    no reply to a 1400-byte ping at MTU 512
    Write Line To Uart    ip stats
    ${f}=    Wait For Line On Uart    frag drop:    timeout=20
    ${line}=    Evaluate    str($f)
    Log    ${line}
    Should Contain    ${line}    df=0 queue=0 pool=0 big=0 tx=0 bad=0 tcp_oversize=0
    Start Emulation
    Should Not Be On Uart    ${FAULT_RE}    timeout=2    treatAsRegex=true

Board TCP Should Follow The MTU
    [Arguments]    ${board}
    [Documentation]    ex41 (TCP echo 7): after 'ip mtu 512' a new connection gets
    ...    MSS 472 both ways, so no segment needs fragmenting. ip_frag_mtu.md 4.3
    Tap Should Be Ready
    Boot Example    ${board}    41    eth=${True}
    ${r}=    Wait For Line On Uart    DHCP BOUND    timeout=60
    ${ip}=    Evaluate    re.search(r'IP=([0-9.]+)', str($r)).group(1)    re
    Start Emulation
    Board Should Answer Ping    ${ip}
    Write Line To Uart    ip mtu 512
    Wait For Line On Uart    MTU set to 512    timeout=20
    Start Emulation
    ${t}=    Run Process    /usr/bin/python3    ${ROOT}/simu/net/frag_check.py
    ...      tcp    ${ip}    7    6000    472    timeout=60s
    Log    ${t.stdout}\n${t.stderr}
    Should Be Equal As Integers    ${t.rc}    0    TCP at MTU 512: ${t.stdout}${t.stderr}
    Should Not Be On Uart    ${FAULT_RE}    timeout=2    treatAsRegex=true

Board VNC Should Serve The Framebuffer
    [Arguments]    ${board}
    [Documentation]    ex139 (fb + VNC on 5900): examples/example_139/vnc_check.py, a raw
    ...    RFB client, sees the pattern in 32/16/8 bpp, a held incremental request,
    ...    a pointer press painted and sent back, a second client refused. The
    ...    client's key event reaches the application callback with its source,
    ...    and a local injection goes through the same callback.
    Tap Should Be Ready
    Boot Example    ${board}    139    eth=${True}
    ${r}=    Wait For Line On Uart    DHCP BOUND    timeout=60
    ${ip}=    Evaluate    re.search(r'IP=([0-9.]+)', str($r)).group(1)    re
    Wait For Line On Uart    VNC listening    timeout=20
    Start Emulation
    Board Should Answer Ping    ${ip}
    # Renode has no CRYP and no RNG: TLS cannot run here. On a part without
    # AES hardware (F767) AUTO must have chosen plain RFB, view only.
    IF    '${board}' == 'f767'
        Write Line To Uart    vnc info
        Wait For Line On Uart    VNC security none (no AES hardware: view only)    timeout=20
        Start Emulation
        Write Line To Uart    e139 input on
        Wait For Line On Uart    E139 client input: on    timeout=20
        Start Emulation
        ${w}=    Run Process    /usr/bin/python3    ${ROOT}/examples/example_139/vnc_check.py
        ...      ${ip}    --view-only    timeout=60s
        Log    ${w.stdout}\n${w.stderr}
        Should Contain    ${w.stdout}    VNC VIEW ONLY PASS    AUTO without AES did not force view only: ${w.stdout}${w.stderr}
    END
    Write Line To Uart    e139 sec none
    Wait For Line On Uart    E139 sec: none    timeout=20
    Start Emulation
    Write Line To Uart    e139 destroy
    Wait For Line On Uart    E139 destroy: ok    timeout=30
    Start Emulation
    Write Line To Uart    e139 new
    Wait For Line On Uart    E139 new: ok    timeout=30
    Start Emulation
    ${v}=    Run Process    /usr/bin/python3    ${ROOT}/examples/example_139/vnc_check.py
    ...      ${ip}    timeout=180s
    Log    ${v.stdout}\n${v.stderr}
    Should Contain    ${v.stdout}    VNC CHECK PASS    VNC check failed: ${v.stdout}${v.stderr}
    Wait For Line On Uart    E139 key src=client down=1 sym=00000061    timeout=10
    Start Emulation
    Write Line To Uart    e139 key 62
    Wait For Line On Uart    E139 key src=local down=1 sym=00000062    timeout=20
    Start Emulation
    Write Line To Uart    vnc info
    Wait For Line On Uart    VNC sessions    timeout=20
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

H753 Example 48 Fragments Over A Reduced MTU
    [Tags]    h753    eth    frag
    Board Should Fragment Over A Reduced MTU    h753

F767 Example 48 Fragments Over A Reduced MTU
    [Tags]    f767    eth    frag
    Board Should Fragment Over A Reduced MTU    f767

H753 Example 41 TCP MSS Follows The MTU
    [Tags]    h753    eth    frag    tcp
    Board TCP Should Follow The MTU    h753

H753 Example 139 VNC Serves The Framebuffer
    [Tags]    h753    eth    vnc    tcp
    Board VNC Should Serve The Framebuffer    h753

F767 Example 139 VNC Serves The Framebuffer
    [Tags]    f767    eth    vnc    tcp
    Board VNC Should Serve The Framebuffer    f767

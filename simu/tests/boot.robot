*** Comments ***
caOS boot smoke test: each board boots the given example and prints the
banner on the console UART, with no fault. Run: simu/test.sh
Each test is tagged with its board: test.sh excludes boards without ELFs, and a
test whose ELF is not built is skipped.

*** Settings ***
Suite Setup       Renode Suite Setup
Suite Teardown    Renode Suite Teardown
Test Teardown     Renode Test Teardown
Resource          ${RENODEKEYWORDS}
Library           OperatingSystem

*** Variables ***
${ROOT}           ${CURDIR}/../..
${PLAT}           ${CURDIR}/../platforms
${GEN}            ${CURDIR}/../out/platforms
&{CONSOLE}        h753=usart3    f767=usart3    f439=usart3    f446=usart3    u575=usart1    h563=usart3    h773=usart3

*** Keywords ***
Boot Example
    [Arguments]    ${board}    ${ex}
    ${mcu}=    Convert To Upper Case    ${board}
    ${elf}=    Set Variable    ${ROOT}/elfs/${mcu}/${board}_example_${ex}.elf
    ${built}=    Run Keyword And Return Status    File Should Exist    ${elf}
    Skip If    not ${built}    ${elf} not built (./do ${ex})
    Execute Command    mach create "${board}"
    Execute Command    machine LoadPlatformDescription @${GEN}/nucleo_${board}.repl
    Execute Command    sysbus LoadELF @${elf}
    Execute Command    include @${PLAT}/nucleo_${board}.resc
    Create Terminal Tester    sysbus.${CONSOLE}[${board}]    timeout=8    defaultPauseEmulation=true
    Start Emulation

Banner Should Appear
    Wait For Line On Uart    -- c.a.O.S --
    Wait For Line On Uart    Core Temperature
    Wait For Line On Uart    Baudrate: 115200 bps

Example 2 Should Run
    [Arguments]    ${board}
    Boot Example    ${board}    2
    Banner Should Appear
    # ex2 is the scheduler demo (02/11/2026): idle at boot, workers on command
    Wait For Line On Uart    example_2: scheduler demo, idle
    Write Line To Uart    same 3 busy
    Wait For Line On Uart    same: all at CA_TH_LEVEL3
    Write Line To Uart    get
    Wait For Line On Uart    round robin:
    Write Line To Uart    stop
    Wait For Line On Uart    stop:
    Should Not Be On Uart    FAULT    timeout=1

MPU Stack Guard Should Fault The Task
    [Arguments]    ${board}
    Boot Example    ${board}    68
    Wait For Line On Uart    -- c.a.O.S --
    Write Line To Uart    fault stack
    Wait For Line On Uart    DACCVIOL: data access violation
    Wait For Line On Uart    HIT LOWER CANARY
    Wait For Line On Uart    Mode: RECOVER (task removed)

*** Test Cases ***
H753 Example 2 Boots
    [Tags]    h753
    Example 2 Should Run    h753

F767 Example 2 Boots
    [Tags]    f767
    Example 2 Should Run    f767

F439 Example 2 Boots
    [Tags]    f439
    Example 2 Should Run    f439

F446 Example 2 Boots
    [Tags]    f446
    Example 2 Should Run    f446

U575 Example 2 Boots
    [Tags]    u575
    Example 2 Should Run    u575

H563 Example 2 Boots
    [Tags]    h563
    Example 2 Should Run    h563

U575 Example 68 MPU Stack Guard
    [Tags]    u575
    MPU Stack Guard Should Fault The Task    u575

H563 Example 68 MPU Stack Guard
    [Tags]    h563
    MPU Stack Guard Should Fault The Task    h563

H773 Example 2 Boots
    [Tags]    h773
    Example 2 Should Run    h773

H773 Example 68 MPU Stack Guard
    [Tags]    h773
    MPU Stack Guard Should Fault The Task    h773

H773 Example 1 Prints Hello
    [Tags]    h773
    Boot Example    h773    1
    Wait For Line On Uart    Hello from CAOS ! E=00000123 V=0456 T=78

H753 Example 1 Prints Hello
    [Tags]    h753
    Boot Example    h753    1
    Wait For Line On Uart    Hello from CAOS ! E=00000123 V=0456 T=78

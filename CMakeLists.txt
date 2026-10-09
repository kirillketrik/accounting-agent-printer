cmake_minimum_required(VERSION 3.20)

# Static CRT (/MT) - must be set before project(). See accounting-agent-core.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(accounting_agent_printer CXX)

# ---- accounting-agent-core ------------------------------------------------
# 1) -DACCOUNTING_AGENT_CORE_DIR=<path> to a local checkout, else
# 2) a sibling ../accounting-agent-core directory, else
# 3) fetched from ACCOUNTING_AGENT_CORE_GIT at ACCOUNTING_AGENT_CORE_TAG.
set(ACCOUNTING_AGENT_CORE_DIR "" CACHE PATH "Local checkout of accounting-agent-core")
set(ACCOUNTING_AGENT_CORE_GIT "https://github.com/kirillketrik/accounting-agent-core.git" CACHE STRING "accounting-agent-core repository")
set(ACCOUNTING_AGENT_CORE_TAG "v1.2.0" CACHE STRING "accounting-agent-core tag/branch (pin a release tag)")
if(NOT ACCOUNTING_AGENT_CORE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../accounting-agent-core/CMakeLists.txt")
    set(ACCOUNTING_AGENT_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../accounting-agent-core")
endif()
if(ACCOUNTING_AGENT_CORE_DIR)
    add_subdirectory("${ACCOUNTING_AGENT_CORE_DIR}" accounting-agent-core)
else()
    include(FetchContent)
    FetchContent_Declare(accounting_agent_core
        GIT_REPOSITORY "${ACCOUNTING_AGENT_CORE_GIT}"
        GIT_TAG        "${ACCOUNTING_AGENT_CORE_TAG}")
    FetchContent_MakeAvailable(accounting_agent_core)
endif()


add_accounting_agent(
    ID             printer
    SERVICE_NAME   PrinterInventoryAgent
    DISPLAY_NAME   "Printer Inventory Agent"
    SOURCES
        src/printer_agent.cpp
        src/printer_config.h
        src/printer_config.cpp
        src/model/printer_info.h
        src/collector/printer_enumerator.h
        src/collector/printer_enumerator.cpp
        src/collector/data_collector.h
        src/collector/data_collector.cpp
        src/collector/liveness_tracker.h
        src/collector/liveness_tracker.cpp
        src/snmp/snmp_client.h
        src/snmp/snmp_client.cpp
        src/snmp/ber.h
        src/snmp/ber.cpp
        src/registry/registry_reader.h
        src/registry/registry_reader.cpp
        src/pjl/pjl_client.h
        src/pjl/pjl_client.cpp
        src/discovery/network_discovery.h
        src/discovery/network_discovery.cpp
    INCLUDE_DIRS   src
    LIBRARIES      winspool      # OpenPrinter/StartDocPrinter/WritePrinter/ReadPrinter (PJL query)
    CONFIG_EXAMPLE config/printer.ini.example
)

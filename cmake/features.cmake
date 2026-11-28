# AIGW OS features chosen when it is built (docs/features.md).
#
# The core is always built: devices, DNS and connection sensing, alerts and the
# domain-risk model, decisions (act), models and signed updates, the data model
# with USP and TR-069, Wi-Fi settings and firmware, Diagnose. Everything else is
# a feature. A feature that is off is not compiled: its commands, data model
# objects, settings and files go too.
#
#   cmake -DAIGW_PROFILE=max ...               a profile's features
#   cmake -DAIGW_FEATURE_MESH=OFF ...          one feature off (or on)
#
# A feature that needs another one off is an error that says which to turn on.
#
# Profiles, each with all of the one before: minimal (the core) < small (8/16 MB
# of flash: what an ISP needs most) < full (the default, 1024 KB of flash) < max
# (every feature, 2048 KB, for 16 MB of flash or more, and PCs).
#
# The rule for a new feature: it goes into max (leave the profile column
# empty). It joins full only when full still fits its 1024 KB with headroom on
# every architecture (CI's flash table), and small only when an ISP needs it on
# the smallest routers.

# name | what it is | the features it needs | the smallest profile it is in (small, full; empty: max)
set(AIGW_FEATURE_TABLE
  "WIFI_HEAL|Self-Healing Wi-Fi, band steering, channel healing, neighbour-aware channels||small"
  "MESH|Smarter mesh Wi-Fi over an EasyMesh controller||full"
  "COVERAGE|Coverage placement advice|WHY|full"
  "TR143|TR-143 diagnostics (IPPing, TraceRoute, Download, Upload, NSLookup)||small"
  "CARE|Proactive care, checks, early warning, the experience score, the speed test|TR143|small"
  "UPGRADE|Firmware upgrades that check themselves and roll back|CARE|small"
  "WHY|Why was it slow, the home's timeline||small"
  "OFFERS|Signals for the ISP (a faster plan, mesh Wi-Fi, and more)|WHY,CARE|full"
  "SQM|Self-tuning smart queueing|WHY|small"
  "CALLS|Calls and games first|SQM|full"
  "BRIEF|The help-desk brief|CARE,WHY|small"
  "PROACTIVE|Proactive care before the call, tickets to the ISP|BRIEF|small"
  "AREA|Area outages, fixes held while many homes fail at once||full"
  "AREAPREDICT|Area fault prediction: line health trends, the shared plant found before it fails|CARE"
  "FED|Federated learning||full"
  "FLEETLEARN|Fleet learning, every home teaches the next (fixes ranked by the fleet's outcomes, warnings from day one)|FED"
  "PLUGINS|Plug-ins and skills (MQTT, Home Assistant, the bridge to cortex)||full"
  "SNI|Connection names from TLS and QUIC handshakes||full"
  "METER|Per-device traffic meter (eBPF)||full"
  "FASTPATH|Kernel fast path (flowtable)||full"
  "REMOTE|Remote access over WireGuard||full"
  "KEYS|Per-device Wi-Fi keys||full"
  "PROVEN|Proven self-repair: every fix predicted, measured, and undone when it didn't help|CARE|small"
)

set(AIGW_PROFILE "full" CACHE STRING "Feature profile: minimal, small, full or max")
set_property(CACHE AIGW_PROFILE PROPERTY STRINGS minimal small full max)
set(AIGW_PROFILES minimal small full max)
# the installed flash each profile must stay within (KB, what aigw-selftest checks)
set(AIGW_BUDGET_minimal 1024)
set(AIGW_BUDGET_small 1024)
set(AIGW_BUDGET_full 1024)
set(AIGW_BUDGET_max 2048)
list(FIND AIGW_PROFILES "${AIGW_PROFILE}" AIGW_PROFILE_RANK)
if(AIGW_PROFILE_RANK LESS 0)
  message(FATAL_ERROR "AIGW_PROFILE is minimal, small, full or max (not ${AIGW_PROFILE})")
endif()

set(AIGW_FEATURE_NAMES "")
foreach(row ${AIGW_FEATURE_TABLE})
  # the empty columns are kept as such (a "-" stands in for them while split)
  string(REPLACE "||" "|-|" row "${row}")
  string(REPLACE "|" ";" f "${row}")
  list(GET f 0 name)
  list(GET f 1 what)
  list(LENGTH f n)
  set(needs "")
  set(joins max)
  if(n GREATER 2)
    list(GET f 2 needs)
    if(needs STREQUAL "-")
      set(needs "")
    endif()
    string(REPLACE "," ";" needs "${needs}")
  endif()
  if(n GREATER 3)
    list(GET f 3 joins)
  endif()
  list(FIND AIGW_PROFILES "${joins}" rank)
  if(rank LESS 1)
    message(FATAL_ERROR "feature ${name}: its profile is small, full or empty (max), not '${joins}'")
  endif()
  if(rank GREATER AIGW_PROFILE_RANK)
    set(dflt OFF)
  else()
    set(dflt ON)
  endif()
  option(AIGW_FEATURE_${name} "${what}" ${dflt})
  set(AIGW_NEEDS_${name} ${needs})
  set(AIGW_JOINS_${name} ${rank})
  list(APPEND AIGW_FEATURE_NAMES ${name})
endforeach()

# what each feature needs must be built too
set(AIGW_FEATURES_ON "")
set(AIGW_FEATURES_LIST "")
set(AIGW_BUILT_RANK 0)          # the smallest profile that has every feature built
foreach(name ${AIGW_FEATURE_NAMES})
  if(AIGW_FEATURE_${name})
    foreach(need ${AIGW_NEEDS_${name}})
      if(NOT AIGW_FEATURE_${need})
        message(FATAL_ERROR "AIGW_FEATURE_${name} needs AIGW_FEATURE_${need}: turn it on, or turn ${name} off")
      endif()
    endforeach()
    list(APPEND AIGW_FEATURES_ON ${name})
    if(AIGW_JOINS_${name} GREATER AIGW_BUILT_RANK)
      set(AIGW_BUILT_RANK ${AIGW_JOINS_${name}})
    endif()
    string(TOLOWER ${name} lower)
    string(REPLACE "_" "-" lower ${lower})
    if(AIGW_FEATURES_LIST)
      set(AIGW_FEATURES_LIST "${AIGW_FEATURES_LIST},${lower}")
    else()
      set(AIGW_FEATURES_LIST "${lower}")
    endif()
  endif()
endforeach()

# the profile this build is (its features are exactly a profile's), else custom; and its flash budget:
# the budget of the smallest profile that has all it has built
set(AIGW_BUILT_PROFILE custom)
foreach(p ${AIGW_PROFILES})
  list(FIND AIGW_PROFILES ${p} r)
  set(same 1)
  foreach(name ${AIGW_FEATURE_NAMES})
    if(AIGW_JOINS_${name} GREATER r)
      set(want 0)
    else()
      set(want 1)
    endif()
    if(AIGW_FEATURE_${name})
      set(have 1)
    else()
      set(have 0)
    endif()
    if(NOT want EQUAL have)
      set(same 0)
    endif()
  endforeach()
  if(same)
    set(AIGW_BUILT_PROFILE ${p})
  endif()
endforeach()
list(GET AIGW_PROFILES ${AIGW_BUILT_RANK} budget_profile)
set(AIGW_FLASH_BUDGET ${AIGW_BUDGET_${budget_profile}})

# ag/features.h: AIGW_FEATURE_<NAME> 1 or 0 for the code, AG_FEATURES the list, the profile and its budget
set(AIGW_FEATURES_H "/* generated by cmake/features.cmake: the features this build has */\n")
string(APPEND AIGW_FEATURES_H "#ifndef AG_FEATURES_H\n#define AG_FEATURES_H\n")
foreach(name ${AIGW_FEATURE_NAMES})
  if(AIGW_FEATURE_${name})
    string(APPEND AIGW_FEATURES_H "#define AIGW_FEATURE_${name} 1\n")
  else()
    string(APPEND AIGW_FEATURES_H "#define AIGW_FEATURE_${name} 0\n")
  endif()
endforeach()
string(APPEND AIGW_FEATURES_H "#define AG_FEATURES \"${AIGW_FEATURES_LIST}\"\n")
string(APPEND AIGW_FEATURES_H "#define AG_PROFILE \"${AIGW_BUILT_PROFILE}\"\n#define AG_FLASH_BUDGET ${AIGW_FLASH_BUDGET}\n#endif\n")
file(WRITE ${PROJECT_BINARY_DIR}/include/ag/features.h.new "${AIGW_FEATURES_H}")
configure_file(${PROJECT_BINARY_DIR}/include/ag/features.h.new ${PROJECT_BINARY_DIR}/include/ag/features.h COPYONLY)
list(LENGTH AIGW_FEATURES_ON n_on)
list(LENGTH AIGW_FEATURE_NAMES n_all)
message(STATUS "AIGW OS features (${n_on} of ${n_all}, profile ${AIGW_BUILT_PROFILE}, flash budget ${AIGW_FLASH_BUDGET} KB): ${AIGW_FEATURES_LIST}")

# sources of a feature: aigw_feature_sources(<list var> <FEATURE> files...)
function(aigw_feature_sources var feature)
  if(AIGW_FEATURE_${feature})
    set(${var} ${${var}} ${ARGN} PARENT_SCOPE)
  endif()
endfunction()

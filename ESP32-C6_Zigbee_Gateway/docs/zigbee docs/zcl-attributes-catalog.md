# Zigbee ZCL Attribute Catalog (ESP Zigbee SDK)

Source: managed_components/espressif__esp-zigbee-lib/include/zcl/*.h (auto-generated from SDK headers).

Purpose: single catalog of cluster -> attribute -> type for frontend and backend.

Limits:
- type is inferred from SDK macros when possible; if not explicit in headers then value is unknown.
- this is a SDK symbol catalog, not full normative ZCL prose spec.

## Summary

- Clusters: **65**
- Attributes: **691**

## 0x0000 Basic cluster identifier. (BASIC)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_BASIC_ZCL_VERSION_ID | ZCL VERSION ID | unknown | ZCL version attribute |
| 0x0001 | ESP_ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID | APPLICATION VERSION ID | unknown | Application version attribute |
| 0x0002 | ESP_ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID | STACK VERSION ID | unknown | Stack version attribute |
| 0x0003 | ESP_ZB_ZCL_ATTR_BASIC_HW_VERSION_ID | HW VERSION ID | unknown | Hardware version attribute |
| 0x0004 | ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID | MANUFACTURER NAME ID | unknown | Manufacturer name attribute |
| 0x0005 | ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID | MODEL IDENTIFIER ID | unknown | Model identifier attribute |
| 0x0006 | ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID | DATE CODE ID | unknown | Date code attribute |
| 0x0007 | ESP_ZB_ZCL_ATTR_BASIC_POWER_SOURCE_ID | POWER SOURCE ID | unknown | Power source attribute |
| 0x0008 | ESP_ZB_ZCL_ATTR_BASIC_GENERIC_DEVICE_CLASS_ID | GENERIC DEVICE CLASS ID | unknown | The GenericDeviceClass attribute defines the field of application of the  GenericDeviceType attribute. |
| 0x0009 | ESP_ZB_ZCL_ATTR_BASIC_GENERIC_DEVICE_TYPE_ID | GENERIC DEVICE TYPE ID | unknown | The GenericDeviceType attribute allows an application to show an icon on a rich user interface (e.g. smartphone app). |
| 0x000A | ESP_ZB_ZCL_ATTR_BASIC_PRODUCT_CODE_ID | PRODUCT CODE ID | unknown | The ProductCode attribute allows an application to specify a code for the product. |
| 0x000B | ESP_ZB_ZCL_ATTR_BASIC_PRODUCT_URL_ID | PRODUCT URL ID | unknown | The ProductURL attribute specifies a link to a web page containing specific product information. |
| 0x000C | ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_VERSION_DETAILS_ID | MANUFACTURER VERSION DETAILS ID | unknown | Vendor specific human readable (displayable) string representing the versions of one of more program images supported on the device. |
| 0x000D | ESP_ZB_ZCL_ATTR_BASIC_SERIAL_NUMBER_ID | SERIAL NUMBER ID | unknown | Vendor specific human readable (displayable) serial number. |
| 0x000E | ESP_ZB_ZCL_ATTR_BASIC_PRODUCT_LABEL_ID | PRODUCT LABEL ID | unknown | Vendor specific human readable (displayable) product label. |
| 0x0010 | ESP_ZB_ZCL_ATTR_BASIC_LOCATION_DESCRIPTION_ID | LOCATION DESCRIPTION ID | unknown | Location description attribute |
| 0x0011 | ESP_ZB_ZCL_ATTR_BASIC_PHYSICAL_ENVIRONMENT_ID | PHYSICAL ENVIRONMENT ID | unknown | Physical environment attribute |
| 0x0012 | ESP_ZB_ZCL_ATTR_BASIC_DEVICE_ENABLED_ID | DEVICE ENABLED ID | unknown | Device enabled attribute |
| 0x0013 | ESP_ZB_ZCL_ATTR_BASIC_ALARM_MASK_ID | ALARM MASK ID | unknown | Alarm mask attribute |
| 0x0014 | ESP_ZB_ZCL_ATTR_BASIC_DISABLE_LOCAL_CONFIG_ID | DISABLE LOCAL CONFIG ID | unknown | Disable local config attribute |
| 0x4000 | ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID | SW BUILD ID | unknown | Manufacturer-specific reference to the version of the software. |

## 0x0001 Power configuration cluster identifier. (POWER_CONFIG)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_MAINS_VOLTAGE_ID | MAINS VOLTAGE ID | unknown | MainsVoltage attribute |
| 0x0001 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_MAINS_FREQUENCY_ID | MAINS FREQUENCY ID | unknown | MainsFrequency attribute |
| 0x0010 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_MAINS_ALARM_MASK_ID | MAINS ALARM MASK ID | unknown |  |
| 0x0011 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_MAINS_VOLTAGE_MIN_THRESHOLD | MAINS VOLTAGE MIN THRESHOLD | uint16_t |  |
| 0x0012 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_MAINS_VOLTAGE_MAX_THRESHOLD | MAINS VOLTAGE MAX THRESHOLD | uint16_t |  |
| 0x0013 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_MAINS_DWELL_TRIP_POINT | MAINS DWELL TRIP POINT | uint16_t |  |
| 0x0020 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID | BATTERY VOLTAGE ID | unknown | BatteryVoltage attribute |
| 0x0021 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID | BATTERY PERCENTAGE REMAINING ID | unknown | BatteryPercentageRemaining attribute |
| 0x0030 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_MANUFACTURER_ID | BATTERY MANUFACTURER ID | unknown | Name of the battery manufacturer as a character string. |
| 0x0031 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_SIZE_ID | BATTERY SIZE ID | unknown | BatterySize attribute |
| 0x0032 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_A_HR_RATING_ID | BATTERY A HR RATING ID | unknown | The Ampere-hour rating of the battery, measured in units of 10mAHr. |
| 0x0033 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_QUANTITY_ID | BATTERY QUANTITY ID | unknown | BatteryQuantity attribute |
| 0x0034 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_RATED_VOLTAGE_ID | BATTERY RATED VOLTAGE ID | unknown | BatteryRatedVoltage attribute |
| 0x0035 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_ALARM_MASK_ID | BATTERY ALARM MASK ID | unknown | BatteryAlarmMask attribute |
| 0x0036 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_MIN_THRESHOLD_ID | BATTERY VOLTAGE MIN THRESHOLD ID | unknown | BatteryVoltageMinThreshold attribute |
| 0x0037 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_THRESHOLD1_ID | BATTERY VOLTAGE THRESHOLD1 ID | unknown | BatteryVoltageThreshold1 attribute |
| 0x0038 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_THRESHOLD2_ID | BATTERY VOLTAGE THRESHOLD2 ID | unknown | BatteryVoltageThreshold2 attribute |
| 0x0039 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_THRESHOLD3_ID | BATTERY VOLTAGE THRESHOLD3 ID | unknown | BatteryVoltageThreshold3 attribute |
| 0x003A | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_MIN_THRESHOLD_ID | BATTERY PERCENTAGE MIN THRESHOLD ID | unknown | BatteryPercentageMinThreshold attribute |
| 0x003B | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_THRESHOLD1_ID | BATTERY PERCENTAGE THRESHOLD1 ID | unknown | BatteryPercentageThreshold1 attribute |
| 0x003C | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_THRESHOLD2_ID | BATTERY PERCENTAGE THRESHOLD2 ID | unknown | BatteryPercentageThreshold2 attribute |
| 0x003D | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_THRESHOLD3_ID | BATTERY PERCENTAGE THRESHOLD3 ID | unknown | BatteryPercentageThreshold3 attribute |
| 0x003E | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_ALARM_STATE_ID | BATTERY ALARM STATE ID | unknown | BatteryAlarmState attribute |
| 0x0040 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY2_VOLTAGE_ID | BATTERY2 VOLTAGE ID | unknown | Battery Information 2 attribute set |
| 0x0041 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY2_PERCENTAGE_REMAINING_ID | BATTERY2 PERCENTAGE REMAINING ID | unknown |  |
| 0x0051 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY2_SIZE_ID | BATTERY2 SIZE ID | unknown |  |
| 0x0053 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY2_QUANTITY_ID | BATTERY2 QUANTITY ID | unknown |  |
| 0x0055 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY2_ALARM_MASK_ID | BATTERY2 ALARM MASK ID | unknown |  |
| 0x0057 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY2_VOLTAGE_THRESHOLD1_ID | BATTERY2 VOLTAGE THRESHOLD1 ID | unknown |  |
| 0x0059 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY2_VOLTAGE_THRESHOLD3_ID | BATTERY2 VOLTAGE THRESHOLD3 ID | unknown |  |
| 0x005B | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY2_PERCENTAGE_THRESHOLD1_ID | BATTERY2 PERCENTAGE THRESHOLD1 ID | unknown |  |
| 0x005D | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY2_PERCENTAGE_THRESHOLD3_ID | BATTERY2 PERCENTAGE THRESHOLD3 ID | unknown |  |
| 0x0060 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY3_VOLTAGE_ID | BATTERY3 VOLTAGE ID | unknown | Battery Information 3 attribute set |
| 0x0061 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY3_PERCENTAGE_REMAINING_ID | BATTERY3 PERCENTAGE REMAINING ID | unknown |  |
| 0x0071 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY3_SIZE_ID | BATTERY3 SIZE ID | unknown |  |
| 0x0073 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY3_QUANTITY_ID | BATTERY3 QUANTITY ID | unknown |  |
| 0x0075 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY3_ALARM_MASK_ID | BATTERY3 ALARM MASK ID | unknown |  |
| 0x0077 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY3_VOLTAGE_THRESHOLD1_ID | BATTERY3 VOLTAGE THRESHOLD1 ID | unknown |  |
| 0x0079 | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY3_VOLTAGE_THRESHOLD3_ID | BATTERY3 VOLTAGE THRESHOLD3 ID | unknown |  |
| 0x007B | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY3_PERCENTAGE_THRESHOLD1_ID | BATTERY3 PERCENTAGE THRESHOLD1 ID | unknown |  |
| 0x007D | ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY3_PERCENTAGE_THRESHOLD3_ID | BATTERY3 PERCENTAGE THRESHOLD3 ID | unknown |  |

## 0x0002 Device temperature configuration cluster identifier. (DEVICE_TEMP_CONFIG)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_DEVICE_TEMP_CONFIG_CURRENT_TEMP_ID | CURRENT TEMP ID | unknown | CurrentTemperature |
| 0x0001 | ESP_ZB_ZCL_ATTR_DEVICE_TEMP_CONFIG_MIN_TEMP_EXPERIENCED_ID | MIN TEMP EXPERIENCED ID | unknown | MinTempExperienced |
| 0x0002 | ESP_ZB_ZCL_ATTR_DEVICE_TEMP_CONFIG_MAX_TEMP_EXPERIENCED_ID | MAX TEMP EXPERIENCED ID | unknown | MaxTempExperienced |
| 0x0003 | ESP_ZB_ZCL_ATTR_DEVICE_TEMP_CONFIG_OVER_TEMP_TOTAL_DWELL_ID | OVER TEMP TOTAL DWELL ID | unknown | OverTempTotalDwell |
| 0x0010 | ESP_ZB_ZCL_ATTR_DEVICE_TEMP_CONFIG_DEVICE_TEMP_ALARM_MASK_ID | DEVICE TEMP ALARM MASK ID | unknown | DeviceTempAlarmMask |
| 0x0011 | ESP_ZB_ZCL_ATTR_DEVICE_TEMP_CONFIG_LOW_TEMP_THRESHOLD_ID | LOW TEMP THRESHOLD ID | unknown | LowTempThreshold |
| 0x0012 | ESP_ZB_ZCL_ATTR_DEVICE_TEMP_CONFIG_HIGH_TEMP_THRESHOLD_ID | HIGH TEMP THRESHOLD ID | unknown | HighTempThreshold |
| 0x0013 | ESP_ZB_ZCL_ATTR_DEVICE_TEMP_CONFIG_LOW_TEMP_DWELL_TRIP_POINT_ID | LOW TEMP DWELL TRIP POINT ID | unknown | LowTempDwellTripPoint |
| 0x0014 | ESP_ZB_ZCL_ATTR_DEVICE_TEMP_CONFIG_HIGH_TEMP_DWELL_TRIP_POINT_ID | HIGH TEMP DWELL TRIP POINT ID | unknown | HighTempDwellTripPoint |

## 0x0003 Identify cluster identifier. (IDENTIFY)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_IDENTIFY_IDENTIFY_TIME_ID | IDENTIFY TIME ID | unknown | Identify time attribute |

## 0x0004 Groups cluster identifier. (GROUPS)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_GROUPS_NAME_SUPPORT_ID | NAME SUPPORT ID | unknown | NameSupport attribute |

## 0x0005 Scenes cluster identifier. (SCENES)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_SCENES_SCENE_COUNT_ID | SCENE COUNT ID | unknown | Number of scenes currently in the device's scene table |
| 0x0001 | ESP_ZB_ZCL_ATTR_SCENES_CURRENT_SCENE_ID | CURRENT SCENE ID | unknown | Scene ID of the scene last invoked |
| 0x0002 | ESP_ZB_ZCL_ATTR_SCENES_CURRENT_GROUP_ID | CURRENT GROUP ID | unknown | Group ID of the scene last invoked |
| 0x0003 | ESP_ZB_ZCL_ATTR_SCENES_SCENE_VALID_ID | SCENE VALID ID | unknown | Indicates whether the state of the device corresponds to CurrentScene and CurrentGroup attributes |
| 0x0004 | ESP_ZB_ZCL_ATTR_SCENES_NAME_SUPPORT_ID | NAME SUPPORT ID | unknown | The most significant bit of the NameSupport attribute indicates whether or not scene names are supported |
| 0x0005 | ESP_ZB_ZCL_ATTR_SCENES_LAST_CONFIGURED_BY_ID | LAST CONFIGURED BY ID | unknown | specifies the IEEE address of the device that last configured the scene table |

## 0x0006 On/Off cluster identifier. (ON_OFF)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID | ON OFF ID | unknown | OnOff attribute |
| 0x4000 | ESP_ZB_ZCL_ATTR_ON_OFF_GLOBAL_SCENE_CONTROL | GLOBAL SCENE CONTROL | bool | Global Scene Control attribute identifier. |
| 0x4001 | ESP_ZB_ZCL_ATTR_ON_OFF_ON_TIME | ON TIME | uint16_t | On Time attribute identifier. |
| 0x4002 | ESP_ZB_ZCL_ATTR_ON_OFF_OFF_WAIT_TIME | OFF WAIT TIME | uint16_t | Off Wait Time attribute identifier. |
| 0x4003 | ESP_ZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF | START UP ON OFF | unknown | Define the desired startup behavior |

## 0x0008 Level control cluster identifier. (LEVEL_CONTROL)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID | CURRENT LEVEL ID | unknown | Current Level attribute |
| 0x0001 | ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_REMAINING_TIME_ID | REMAINING TIME ID | unknown | Remaining Time attribute |
| 0x0002 | ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_MIN_LEVEL_ID | MIN LEVEL ID | unknown | The MinLevel attribute indicates the minimum value of CurrentLevel that is capable of being assigned. |
| 0x0003 | ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_MAX_LEVEL_ID | MAX LEVEL ID | unknown | The MaxLevel attribute indicates the maximum value of CurrentLevel that is capable of being assigned. |
| 0x0004 | ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_FREQUENCY_ID | CURRENT FREQUENCY ID | unknown | The CurrentFrequency attribute represents the frequency that the devices is at CurrentLevel. |
| 0x0005 | ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_MIN_FREQUENCY_ID | MIN FREQUENCY ID | unknown | The MinFrequency attribute indicates the minimum value of CurrentFrequency that is capable of being assigned. |
| 0x0006 | ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_MAX_FREQUENCY_ID | MAX FREQUENCY ID | unknown | The MaxFrequency attribute indicates the maximum value of CurrentFrequency that is capable of being assigned. |
| 0x000F | ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_OPTIONS_ID | OPTIONS ID | unknown | The Options attribute is a bitmap that determines the default behavior of some cluster commands. |
| 0x0010 | ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_ON_OFF_TRANSITION_TIME_ID | ON OFF TRANSITION TIME ID | unknown | On off transition time attribute |
| 0x0011 | ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_ON_LEVEL_ID | ON LEVEL ID | unknown | On Level attribute |
| 0x0012 | ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_ON_TRANSITION_TIME_ID | ON TRANSITION TIME ID | unknown | The OnTransitionTime attribute represents the time taken to move the current level |
| 0x0013 | ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_OFF_TRANSITION_TIME_ID | OFF TRANSITION TIME ID | unknown | The OffTransitionTime attribute represents the time taken to move the current level |
| 0x0014 | ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_DEFAULT_MOVE_RATE_ID | DEFAULT MOVE RATE ID | unknown | The DefaultMoveRate attribute determines the movement rate, in units per second |
| 0x4000 | ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_START_UP_CURRENT_LEVEL_ID | START UP CURRENT LEVEL ID | unknown | The StartUpCurrentLevel attribute SHALL define the desired startup level |
| 0xEFFF | ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_MOVE_STATUS_ID | MOVE STATUS ID | unknown | Special Move Variables attribute Internal usage |

## 0x0009 Alarms cluster identifier. (ALARMS)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_ALARMS_ALARM_COUNT_ID | ALARM COUNT ID | unknown | AlarmCount attribute |
| 0xEFFE | ESP_ZB_ZCL_ATTR_ALARMS_ALARM_TABLE_SIZE_ID | ALARM TABLE SIZE ID | unknown | Internal AlarmTable size attribute |
| 0xEFFF | ESP_ZB_ZCL_ATTR_ALARMS_ALARM_TABLE_ID | ALARM TABLE ID | unknown | Internal AlarmTable attribute |

## 0x000A Time cluster identifier. (TIME)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_TIME_TIME_ID | TIME ID | unknown | Time attribute |
| 0x0001 | ESP_ZB_ZCL_ATTR_TIME_TIME_STATUS_ID | TIME STATUS ID | unknown | Time Status attribute |
| 0x0002 | ESP_ZB_ZCL_ATTR_TIME_TIME_ZONE_ID | TIME ZONE ID | unknown | Time Zone attribute |
| 0x0003 | ESP_ZB_ZCL_ATTR_TIME_DST_START_ID | DST START ID | unknown | Dst Start attribute |
| 0x0004 | ESP_ZB_ZCL_ATTR_TIME_DST_END_ID | DST END ID | unknown | Dst End attribute |
| 0x0005 | ESP_ZB_ZCL_ATTR_TIME_DST_SHIFT_ID | DST SHIFT ID | unknown | Dst Shift attribute |
| 0x0006 | ESP_ZB_ZCL_ATTR_TIME_STANDARD_TIME_ID | STANDARD TIME ID | unknown | Standard Time attribute |
| 0x0007 | ESP_ZB_ZCL_ATTR_TIME_LOCAL_TIME_ID | LOCAL TIME ID | unknown | Local Time attribute |
| 0x0008 | ESP_ZB_ZCL_ATTR_TIME_LAST_SET_TIME_ID | LAST SET TIME ID | unknown | Last Set Time attribute |
| 0x0009 | ESP_ZB_ZCL_ATTR_TIME_VALID_UNTIL_TIME_ID | VALID UNTIL TIME ID | unknown | Valid Until Time attribute |

## 0x000C Analog input (basic) cluster identifier. (ANALOG_INPUT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x001C | ESP_ZB_ZCL_ATTR_ANALOG_INPUT_DESCRIPTION_ID | DESCRIPTION ID | unknown | Description attribute |
| 0x0041 | ESP_ZB_ZCL_ATTR_ANALOG_INPUT_MAX_PRESENT_VALUE_ID | MAX PRESENT VALUE ID | unknown | MaxPresentValue attribute |
| 0x0045 | ESP_ZB_ZCL_ATTR_ANALOG_INPUT_MIN_PRESENT_VALUE_ID | MIN PRESENT VALUE ID | unknown | MinPresentValue attribute |
| 0x0051 | ESP_ZB_ZCL_ATTR_ANALOG_INPUT_OUT_OF_SERVICE_ID | OUT OF SERVICE ID | unknown | OutOfService attribute |
| 0x0055 | ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID | PRESENT VALUE ID | unknown | PresentValue attribute |
| 0x0067 | ESP_ZB_ZCL_ATTR_ANALOG_INPUT_RELIABILITY_ID | RELIABILITY ID | unknown | Reliability attribute |
| 0x006A | ESP_ZB_ZCL_ATTR_ANALOG_INPUT_RESOLUTION_ID | RESOLUTION ID | unknown | Resolution attribute |
| 0x006F | ESP_ZB_ZCL_ATTR_ANALOG_INPUT_STATUS_FLAGS_ID | STATUS FLAGS ID | unknown | StatusFlags attribute |
| 0x0075 | ESP_ZB_ZCL_ATTR_ANALOG_INPUT_ENGINEERING_UNITS_ID | ENGINEERING UNITS ID | unknown | EngineeringUnits attribute |
| 0x0100 | ESP_ZB_ZCL_ATTR_ANALOG_INPUT_APPLICATION_TYPE_ID | APPLICATION TYPE ID | unknown | ApplicationType attribute |

## 0x000D Analog output (basic) cluster identifier. (ANALOG_OUTPUT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x001C | ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_DESCRIPTION_ID | DESCRIPTION ID | unknown | Description attribute |
| 0x0041 | ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_MAX_PRESENT_VALUE_ID | MAX PRESENT VALUE ID | unknown | MaxPresentValue attribute |
| 0x0045 | ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_MIN_PRESENT_VALUE_ID | MIN PRESENT VALUE ID | unknown | MinPresentValue attribute |
| 0x0051 | ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_OUT_OF_SERVICE_ID | OUT OF SERVICE ID | unknown | OutOfService attribute |
| 0x0055 | ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_PRESENT_VALUE_ID | PRESENT VALUE ID | unknown | PresentValue attribute |
| 0x0057 | ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_PRIORITY_ARRAY_ID | PRIORITY ARRAY ID | unknown | PriorityArray attribute |
| 0x0067 | ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_RELIABILITY_ID | RELIABILITY ID | unknown | Reliability attribute |
| 0x0068 | ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_RELINQUISH_DEFAULT_ID | RELINQUISH DEFAULT ID | unknown | RelinquishDefault attribute |
| 0x006A | ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_RESOLUTION_ID | RESOLUTION ID | unknown | Resolution attribute |
| 0x006F | ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_STATUS_FLAGS_ID | STATUS FLAGS ID | unknown | StatusFlags attribute |
| 0x0075 | ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_ENGINEERING_UNITS_ID | ENGINEERING UNITS ID | unknown | EngineeringUnits attribute |
| 0x0100 | ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_APPLICATION_TYPE_ID | APPLICATION TYPE ID | unknown | ApplicationType attribute |

## 0x000E Analog value (basic) cluster identifier. (ANALOG_VALUE)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x001C | ESP_ZB_ZCL_ATTR_ANALOG_VALUE_DESCRIPTION_ID | DESCRIPTION ID | unknown | Description attribute |
| 0x0051 | ESP_ZB_ZCL_ATTR_ANALOG_VALUE_OUT_OF_SERVICE_ID | OUT OF SERVICE ID | unknown | OutOfService attribute |
| 0x0055 | ESP_ZB_ZCL_ATTR_ANALOG_VALUE_PRESENT_VALUE_ID | PRESENT VALUE ID | unknown | PresentValue attribute |
| 0x0057 | ESP_ZB_ZCL_ATTR_ANALOG_VALUE_PRIORITY_ARRAY_ID | PRIORITY ARRAY ID | unknown | PriorityArray attribute |
| 0x0067 | ESP_ZB_ZCL_ATTR_ANALOG_VALUE_RELIABILITY_ID | RELIABILITY ID | unknown | Reliability attribute |
| 0x0068 | ESP_ZB_ZCL_ATTR_ANALOG_VALUE_RELINQUISH_DEFAULT_ID | RELINQUISH DEFAULT ID | unknown | RelinquishDefault attribute |
| 0x006F | ESP_ZB_ZCL_ATTR_ANALOG_VALUE_STATUS_FLAGS_ID | STATUS FLAGS ID | unknown | StatusFlags attribute |
| 0x0075 | ESP_ZB_ZCL_ATTR_ANALOG_VALUE_ENGINEERING_UNITS_ID | ENGINEERING UNITS ID | unknown | EngineeringUnits attribute |
| 0x0100 | ESP_ZB_ZCL_ATTR_ANALOG_VALUE_APPLICATION_TYPE_ID | APPLICATION TYPE ID | unknown | ApplicationType attribute |

## 0x000F Binary input (basic) cluster identifier. (BINARY_INPUT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0004 | ESP_ZB_ZCL_ATTR_BINARY_INPUT_ACTIVE_TEXT_ID | ACTIVE TEXT ID | unknown | This attribute holds a human readable description of the ACTIVE state of a binary PresentValue. |
| 0x001C | ESP_ZB_ZCL_ATTR_BINARY_INPUT_DESCRIPTION_ID | DESCRIPTION ID | unknown | The description of the usage of the input, output or value, as appropriate to the cluster. |
| 0x002E | ESP_ZB_ZCL_ATTR_BINARY_INPUT_INACTIVE_TEXT_ID | INACTIVE TEXT ID | unknown | This attribute holds a human readable description of the INACTIVE state of a binary PresentValue. |
| 0x0051 | ESP_ZB_ZCL_ATTR_BINARY_INPUT_OUT_OF_SERVICE_ID | OUT OF SERVICE ID | unknown | OutOfService attribute |
| 0x0054 | ESP_ZB_ZCL_ATTR_BINARY_INPUT_POLARITY_ID | POLARITY ID | unknown | This attribute indicates the relationship between the physical state of the input (or output as appropriate for the cluster) and the logical state represented by a binary PresentValue attribute, when OutOfService is FALSE. |
| 0x0055 | ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID | PRESENT VALUE ID | unknown | PresentValue attribute |
| 0x0067 | ESP_ZB_ZCL_ATTR_BINARY_INPUT_RELIABILITY_ID | RELIABILITY ID | unknown | The attribute indicates whether the PresentValue or the operation of the physical input, output or value in question (as appropriate for the cluster) is reliable. |
| 0x006F | ESP_ZB_ZCL_ATTR_BINARY_INPUT_STATUS_FLAGS_ID | STATUS FLAGS ID | unknown | StatusFlag attribute |
| 0x0100 | ESP_ZB_ZCL_ATTR_BINARY_INPUT_APPLICATION_TYPE_ID | APPLICATION TYPE ID | unknown | The attribute indicates the specific application usage for this cluster. |

## 0x0010 Binary output (basic) cluster identifier. (BINARY_OUTPUT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0004 | ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_ACTIVE_TEXT_ID | ACTIVE TEXT ID | unknown |  |
| 0x001C | ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_DESCRIPTION_ID | DESCRIPTION ID | unknown |  |
| 0x002E | ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_INACTIVE_TEXT_ID | INACTIVE TEXT ID | unknown |  |
| 0x0042 | ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_MIN_OFF_TIME_ID | MIN OFF TIME ID | unknown |  |
| 0x0043 | ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_MAX_ON_TIME_ID | MAX ON TIME ID | unknown |  |
| 0x0051 | ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_OUT_OF_SERVICE_ID | OUT OF SERVICE ID | unknown |  |
| 0x0054 | ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_POLARITY_ID | POLARITY ID | unknown |  |
| 0x0055 | ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_PRESENT_VALUE_ID | PRESENT VALUE ID | unknown |  |
| 0x0067 | ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_RELIABILITY_ID | RELIABILITY ID | unknown |  |
| 0x0068 | ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_RELINQUISH_DEFAULT_ID | RELINQUISH DEFAULT ID | unknown |  |
| 0x006F | ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_STATUS_FLAGS_ID | STATUS FLAGS ID | unknown |  |
| 0x0100 | ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_APPLICATION_TYPE_ID | APPLICATION TYPE ID | unknown |  |

## 0x0011 Binary value (basic) cluster identifier. (BINARY_VALUE)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0004 | ESP_ZB_ZCL_ATTR_BINARY_VALUE_ACTIVE_TEXT_ID | ACTIVE TEXT ID | unknown |  |
| 0x001C | ESP_ZB_ZCL_ATTR_BINARY_VALUE_DESCRIPTION_ID | DESCRIPTION ID | unknown |  |
| 0x002E | ESP_ZB_ZCL_ATTR_BINARY_VALUE_INACTIVE_TEXT_ID | INACTIVE TEXT ID | unknown |  |
| 0x0042 | ESP_ZB_ZCL_ATTR_BINARY_VALUE_MIN_OFF_TIME_ID | MIN OFF TIME ID | unknown |  |
| 0x0043 | ESP_ZB_ZCL_ATTR_BINARY_VALUE_MIN_ON_TIME_ID | MIN ON TIME ID | unknown |  |
| 0x0051 | ESP_ZB_ZCL_ATTR_BINARY_VALUE_OUT_OF_SERVICE_ID | OUT OF SERVICE ID | unknown |  |
| 0x0055 | ESP_ZB_ZCL_ATTR_BINARY_VALUE_PRESENT_VALUE_ID | PRESENT VALUE ID | unknown |  |
| 0x0067 | ESP_ZB_ZCL_ATTR_BINARY_VALUE_RELIABILITY_ID | RELIABILITY ID | unknown |  |
| 0x0068 | ESP_ZB_ZCL_ATTR_BINARY_VALUE_RELINQUISH_DEFAULT_ID | RELINQUISH DEFAULT ID | unknown |  |
| 0x006F | ESP_ZB_ZCL_ATTR_BINARY_VALUE_STATUS_FLAGS_ID | STATUS FLAGS ID | unknown |  |
| 0x0100 | ESP_ZB_ZCL_ATTR_BINARY_VALUE_APPLICATION_TYPE_ID | APPLICATION TYPE ID | unknown |  |

## 0x0012 Multistate input (basic) cluster identifier. (MULTI_INPUT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x000E | ESP_ZB_ZCL_ATTR_MULTI_INPUT_STATE_TEXT_ID | STATE TEXT ID | unknown |  |
| 0x001C | ESP_ZB_ZCL_ATTR_MULTI_INPUT_DESCRIPTION_ID | DESCRIPTION ID | unknown |  |
| 0x004A | ESP_ZB_ZCL_ATTR_MULTI_INPUT_NUMBER_OF_STATES_ID | NUMBER OF STATES ID | unknown |  |
| 0x0051 | ESP_ZB_ZCL_ATTR_MULTI_INPUT_OUT_OF_SERVICE_ID | OUT OF SERVICE ID | unknown |  |
| 0x0055 | ESP_ZB_ZCL_ATTR_MULTI_INPUT_PRESENT_VALUE_ID | PRESENT VALUE ID | unknown |  |
| 0x0067 | ESP_ZB_ZCL_ATTR_MULTI_INPUT_RELIABILITY_ID | RELIABILITY ID | unknown |  |
| 0x006F | ESP_ZB_ZCL_ATTR_MULTI_INPUT_STATUS_FLAGS_ID | STATUS FLAGS ID | unknown |  |
| 0x0100 | ESP_ZB_ZCL_ATTR_MULTI_INPUT_APPLICATION_TYPE_ID | APPLICATION TYPE ID | unknown |  |

## 0x0013 Multistate output (basic) cluster identifier. (MULTI_OUTPUT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x000E | ESP_ZB_ZCL_ATTR_MULTI_OUTPUT_STATE_TEXT_ID | STATE TEXT ID | unknown | StateText attribute |
| 0x001C | ESP_ZB_ZCL_ATTR_MULTI_OUTPUT_DESCRIPTION_ID | DESCRIPTION ID | unknown | Description attribute |
| 0x004A | ESP_ZB_ZCL_ATTR_MULTI_OUTPUT_NUMBER_OF_STATES_ID | NUMBER OF STATES ID | unknown | Number of states attribute |
| 0x0051 | ESP_ZB_ZCL_ATTR_MULTI_OUTPUT_OUT_OF_SERVICE_ID | OUT OF SERVICE ID | unknown | OutOfService attribute |
| 0x0055 | ESP_ZB_ZCL_ATTR_MULTI_OUTPUT_PRESENT_VALUE_ID | PRESENT VALUE ID | unknown | PresentValue attribute |
| 0x0067 | ESP_ZB_ZCL_ATTR_MULTI_OUTPUT_RELIABILITY_ID | RELIABILITY ID | unknown | Reliability attribute |
| 0x0068 | ESP_ZB_ZCL_ATTR_MULTI_OUTPUT_RELINQUISH_DEFAULT_ID | RELINQUISH DEFAULT ID | unknown | Relinquish default attribute |
| 0x006F | ESP_ZB_ZCL_ATTR_MULTI_OUTPUT_STATUS_FLAGS_ID | STATUS FLAGS ID | unknown | StatusFlag attribute |
| 0x0100 | ESP_ZB_ZCL_ATTR_MULTI_OUTPUT_APPLICATION_TYPE_ID | APPLICATION TYPE ID | unknown | Application type attribute |

## 0x0014 Multistate value (basic) cluster identifier. (MULTI_VALUE)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x000E | ESP_ZB_ZCL_ATTR_MULTI_VALUE_STATE_TEXT_ID | STATE TEXT ID | unknown | Text attribute |
| 0x001C | ESP_ZB_ZCL_ATTR_MULTI_VALUE_DESCRIPTION_ID | DESCRIPTION ID | unknown | Description attribute |
| 0x004A | ESP_ZB_ZCL_ATTR_MULTI_VALUE_NUMBER_OF_STATES_ID | NUMBER OF STATES ID | unknown | NumberOfStates attribute |
| 0x0051 | ESP_ZB_ZCL_ATTR_MULTI_VALUE_OUT_OF_SERVICE_ID | OUT OF SERVICE ID | unknown | OutOfService attribute |
| 0x0055 | ESP_ZB_ZCL_ATTR_MULTI_VALUE_PRESENT_VALUE_ID | PRESENT VALUE ID | unknown | PresentValue attribute |
| 0x0067 | ESP_ZB_ZCL_ATTR_MULTI_VALUE_RELIABILITY_ID | RELIABILITY ID | unknown | Reliability attribute |
| 0x0068 | ESP_ZB_ZCL_ATTR_MULTI_VALUE_RELINQUISH_DEFAULT_ID | RELINQUISH DEFAULT ID | unknown | Reliability attribute |
| 0x006F | ESP_ZB_ZCL_ATTR_MULTI_VALUE_STATUS_FLAGS_ID | STATUS FLAGS ID | unknown | StatusFlags attribute |
| 0x0100 | ESP_ZB_ZCL_ATTR_MULTI_VALUE_APPLICATION_TYPE_ID | APPLICATION TYPE ID | unknown | ApplicationType attribute |

## 0x0015 Commissioning cluster identifier. (COMMISSIONING)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_COMMISSIONING_SHORT_ADDRESS_ID | SHORT ADDRESS ID | unknown |  |
| 0x0000 | ESP_ZB_ZCL_ATTR_COMMISSIONING_STARTUP_TYPE_JOINED | STARTUP TYPE JOINED | unknown |  |
| 0x0001 | ESP_ZB_ZCL_ATTR_COMMISSIONING_EXTENDED_PANID_ID | EXTENDED PANID ID | unknown |  |
| 0x0002 | ESP_ZB_ZCL_ATTR_COMMISSIONING_PANID_ID | PANID ID | unknown |  |
| 0x0002 | ESP_ZB_ZCL_ATTR_COMMISSIONING_STARTUP_TYPE_REJOIN | STARTUP TYPE REJOIN | unknown |  |
| 0x0003 | ESP_ZB_ZCL_ATTR_COMMISSIONING_CHANNEL_MASK_ID | CHANNEL MASK ID | unknown |  |
| 0x0004 | ESP_ZB_ZCL_ATTR_COMMISSIONING_PROTOCOL_VERSION_ID | PROTOCOL VERSION ID | unknown |  |
| 0x0005 | ESP_ZB_ZCL_ATTR_COMMISSIONING_STACK_PROFILE_ID | STACK PROFILE ID | unknown |  |
| 0x0006 | ESP_ZB_ZCL_ATTR_COMMISSIONING_STARTUP_CONTROL_ID | STARTUP CONTROL ID | unknown |  |
| 0x0010 | ESP_ZB_ZCL_ATTR_COMMISSIONING_TRUST_CENTER_ADDRESS_ID | TRUST CENTER ADDRESS ID | unknown |  |
| 0x0011 | ESP_ZB_ZCL_ATTR_COMMISSIONING_TRUST_CENTER_MASTER_KEY_ID | TRUST CENTER MASTER KEY ID | unknown |  |
| 0x0012 | ESP_ZB_ZCL_ATTR_COMMISSIONING_NETWORK_KEY_ID | NETWORK KEY ID | unknown |  |
| 0x0013 | ESP_ZB_ZCL_ATTR_COMMISSIONING_USE_INSECURE_JOIN_ID | USE INSECURE JOIN ID | unknown |  |
| 0x0014 | ESP_ZB_ZCL_ATTR_COMMISSIONING_PRECONFIGURED_LINK_KEY_ID | PRECONFIGURED LINK KEY ID | unknown |  |
| 0x0015 | ESP_ZB_ZCL_ATTR_COMMISSIONING_NETWORK_KEY_SEQ_NUM_ID | NETWORK KEY SEQ NUM ID | unknown |  |
| 0x0016 | ESP_ZB_ZCL_ATTR_COMMISSIONING_NETWORK_KEY_TYPE_ID | NETWORK KEY TYPE ID | unknown |  |
| 0x0017 | ESP_ZB_ZCL_ATTR_COMMISSIONING_NETWORK_MANAGER_ADDRESS_ID | NETWORK MANAGER ADDRESS ID | unknown |  |
| 0x0020 | ESP_ZB_ZCL_ATTR_COMMISSIONING_SCAN_ATTEMPTS_ID | SCAN ATTEMPTS ID | unknown |  |
| 0x0021 | ESP_ZB_ZCL_ATTR_COMMISSIONING_TIME_BETWEEN_SCANS_ID | TIME BETWEEN SCANS ID | unknown |  |
| 0x0022 | ESP_ZB_ZCL_ATTR_COMMISSIONING_REJOIN_INTERVAL_ID | REJOIN INTERVAL ID | unknown |  |
| 0x0023 | ESP_ZB_ZCL_ATTR_COMMISSIONING_MAX_REJOIN_INTERVAL_ID | MAX REJOIN INTERVAL ID | unknown |  |
| 0x0030 | ESP_ZB_ZCL_ATTR_COMMISSIONING_INDIRECT_POLL_RATE_ID | INDIRECT POLL RATE ID | unknown |  |
| 0x0031 | ESP_ZB_ZCL_ATTR_COMMISSIONING_PARENT_RETRY_THRESHOLD_ID | PARENT RETRY THRESHOLD ID | unknown |  |
| 0x0040 | ESP_ZB_ZCL_ATTR_COMMISSIONING_CONCENTRATOR_FLAG_ID | CONCENTRATOR FLAG ID | unknown |  |
| 0x0041 | ESP_ZB_ZCL_ATTR_COMMISSIONING_CONCENTRATOR_RADIUS_ID | CONCENTRATOR RADIUS ID | unknown |  |
| 0x0042 | ESP_ZB_ZCL_ATTR_COMMISSIONING_CONCENTRATOR_DISCOVERY_TIME_ID | CONCENTRATOR DISCOVERY TIME ID | unknown |  |

## 0x0019 Over The Air cluster identifier. (OTA_UPGRADE)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ID | SERVER ID | unknown | Indicates the address of the upgrade server |
| 0x0001 | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_FILE_OFFSET_ID | FILE OFFSET ID | unknown | Indicates the the current location in the OTA upgrade image |
| 0x0002 | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_FILE_VERSION_ID | FILE VERSION ID | unknown | Indicates the file version of the running firmware image on the device |
| 0x0003 | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_STACK_VERSION_ID | STACK VERSION ID | unknown | Brief CurrentZigbeeStackVersion attribute |
| 0x0004 | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_DOWNLOADED_FILE_VERSION_ID | DOWNLOADED FILE VERSION ID | unknown | Indicates the file version of the downloaded image on the device |
| 0x0005 | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_DOWNLOADED_STACK_VERSION_ID | DOWNLOADED STACK VERSION ID | unknown | Brief DownloadedZigbeeStackVersion attribute |
| 0x0006 | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_IMAGE_STATUS_ID | IMAGE STATUS ID | unknown | Indicates the image upgrade status of the client device |
| 0x0007 | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_MANUFACTURE_ID | MANUFACTURE ID | unknown | Indicates the value for the manufacturer of the device |
| 0x0008 | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_IMAGE_TYPE_ID | IMAGE TYPE ID | unknown | Indicates the the image type of the file that the client is currently downloading |
| 0x0009 | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_MIN_BLOCK_REQUE_ID | MIN BLOCK REQUE ID | unknown | Indicates the delay between Image Block Request commands in milliseconds |
| 0x000A | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_IMAGE_STAMP_ID | IMAGE STAMP ID | unknown | Brief Image Stamp attribute |
| 0x000B | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_UPGRADE_ACTIVATION_POLICY_ID | UPGRADE ACTIVATION POLICY ID | unknown | Indicates what behavior the client device supports for activating a fully downloaded but not installed upgrade image |
| 0x000C | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_UPGRADE_TIMEOUT_POLICY_ID | UPGRADE TIMEOUT POLICY ID | unknown | Indicates what behavior the client device supports for activating a fully downloaded image when the OTA server cannot be reached |
| 0xFFF0 | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_DATA_ID | SERVER DATA ID | unknown | Brief OTA server data attribute, its type can refer to esp_zb_zcl_ota_upgrade_server_variable_t |
| 0xFFF1 | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID | CLIENT DATA ID | unknown | Brief OTA client data attribute, its type can refer to esp_zb_zcl_ota_upgrade_client_variable_t |
| 0xFFF2 | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ADDR_ID | SERVER ADDR ID | unknown | Brief OTA server addr custom attribute |
| 0xFFF3 | ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ENDPOINT_ID | SERVER ENDPOINT ID | unknown | Brief OTA server endpoint custom attribute |

## 0x0020 Poll control cluster identifier. (POLL_CONTROL)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_POLL_CONTROL_CHECK_IN_INTERVAL_ID | CHECK IN INTERVAL ID | unknown |  |
| 0x0001 | ESP_ZB_ZCL_ATTR_POLL_CONTROL_LONG_POLL_INTERVAL_ID | LONG POLL INTERVAL ID | unknown |  |
| 0x0002 | ESP_ZB_ZCL_ATTR_POLL_CONTROL_SHORT_POLL_INTERVAL_ID | SHORT POLL INTERVAL ID | unknown |  |
| 0x0003 | ESP_ZB_ZCL_ATTR_POLL_CONTROL_FAST_POLL_TIMEOUT_ID | FAST POLL TIMEOUT ID | unknown |  |
| 0x0004 | ESP_ZB_ZCL_ATTR_POLL_CONTROL_MIN_CHECK_IN_INTERVAL_ID | MIN CHECK IN INTERVAL ID | unknown |  |
| 0x0005 | ESP_ZB_ZCL_ATTR_POLL_CONTROL_LONG_POLL_MIN_INTERVAL_ID | LONG POLL MIN INTERVAL ID | unknown |  |
| 0x0006 | ESP_ZB_ZCL_ATTR_POLL_CONTROL_FAST_POLL_MAX_TIMEOUT_ID | FAST POLL MAX TIMEOUT ID | unknown |  |

## 0x0100 Shade configuration cluster identifier. (SHADE_CONFIG)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_SHADE_CONFIG_PHYSICAL_CLOSED_LIMIT_ID | PHYSICAL CLOSED LIMIT ID | unknown | It indicates the most closed (numerically lowest) position that the shade can physically move to. |
| 0x0001 | ESP_ZB_ZCL_ATTR_SHADE_CONFIG_MOTOR_STEP_SIZE_ID | MOTOR STEP SIZE ID | unknown | It indicates the angle the shade motor moves for one step, measured in 1/10ths of a degree. |
| 0x0002 | ESP_ZB_ZCL_ATTR_SHADE_CONFIG_STATUS_ID | STATUS ID | unknown | Status attribute |

## 0x0101 Door lock cluster identifier. (DOOR_LOCK)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_LOCK_STATE_ID | LOCK STATE ID | unknown | brief LockState attribute |
| 0x0001 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_LOCK_TYPE_ID | LOCK TYPE ID | unknown | brief LockType attribute |
| 0x0002 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_ACTUATOR_ENABLED_ID | ACTUATOR ENABLED ID | unknown | brief ActuatorEnabled attribute |
| 0x0003 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_DOOR_STATE_ID | DOOR STATE ID | unknown | brief DoorState attribute |
| 0x0004 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_NUM_OF_DOOR_OPEN_EVENTS_ID | NUM OF DOOR OPEN EVENTS ID | unknown | brief DoorOpenEvents attribute |
| 0x0005 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_NUM_OF_DOOR_CLOSED_EVENTS_ID | NUM OF DOOR CLOSED EVENTS ID | unknown | brief DoorClosedEvents attribute |
| 0x0006 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_OPEN_PERIOD_ID | OPEN PERIOD ID | unknown | brief OpenPeriod attribute |
| 0x0010 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_NUMBER_OF_LOG_RECORDS_SUPPORTED_ID | NUMBER OF LOG RECORDS SUPPORTED ID | unknown | The number of available log records. |
| 0x0011 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_NUM_TOTAL_USERS_ID | NUM TOTAL USERS ID | unknown | brief NumberOfTotalUsersSupported attribute |
| 0x0012 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_NUM_PIN_USERS_ID | NUM PIN USERS ID | unknown | brief NumberOfPINUsersSupported attribute |
| 0x0013 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_NUMBER_OF_RFID_USERS_SUPPORTED_ID | NUMBER OF RFID USERS SUPPORTED ID | unknown | he number of RFID users supported. |
| 0x0014 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_NUM_WEEK_DAY_SCHEDULE_PER_USER_ID | NUM WEEK DAY SCHEDULE PER USER ID | unknown | brief NumberOfWeekDaySchedulesSupportedPerUser attribute |
| 0x0015 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_NUM_YEAR_DAY_SCHEDULE_PER_USER_ID | NUM YEAR DAY SCHEDULE PER USER ID | unknown | brief NumberOfYearDaySchedulesSupportedPerUser attribute |
| 0x0016 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_NUM_HOLIDAY_SCHEDULE_ID | NUM HOLIDAY SCHEDULE ID | unknown | brief NumberOfHolidaySchedulesSupported attribute |
| 0x0017 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_MAX_PIN_LEN_ID | MAX PIN LEN ID | unknown | brief Max PIN code length attribute |
| 0x0018 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_MIN_PIN_LEN_ID | MIN PIN LEN ID | unknown | brief Min PIN code length attribute |
| 0x0019 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_MAX_RFID_CODE_LENGTH_ID | MAX RFID CODE LENGTH ID | unknown | An 8-bit value indicates the maximum length in bytes of a RFID Code on this device. |
| 0x001A | ESP_ZB_ZCL_ATTR_DOOR_LOCK_MIN_RFID_CODE_LENGTH_ID | MIN RFID CODE LENGTH ID | unknown | An 8-bit value indicates the minimum length in bytes of a RFID Code on this device. |
| 0x0020 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_ENABLE_LOGGING_ID | ENABLE LOGGING ID | unknown | Enable/disable event logging. |
| 0x0021 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_LANGUAGE_ID | LANGUAGE ID | unknown | Modifies the language for the on-screen or audible user interface using three bytes from ISO-639-1. |
| 0x0022 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_LED_SETTINGS_ID | LED SETTINGS ID | unknown | The settings for the LED support three different modes. |
| 0x0023 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_AUTO_RELOCK_TIME_ID | AUTO RELOCK TIME ID | unknown | The number of seconds to wait after unlocking a lock before it automatically locks again. |
| 0x0024 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_SOUND_VOLUME_ID | SOUND VOLUME ID | unknown | The sound volume on a door lock has three possible settings: silent, low and high volumes. |
| 0x0025 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_OPERATING_MODE_ID | OPERATING MODE ID | unknown | OperatingMode attribute |
| 0x0026 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_OPERATION_MODES_SUPPORTED_ID | OPERATION MODES SUPPORTED ID | unknown | SupportedOperatingModes attribute |
| 0x0027 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_DEFAULT_CONFIGURATION_REGISTER_ID | DEFAULT CONFIGURATION REGISTER ID | unknown | This attribute represents the default configurations as they are physically set on the device |
| 0x0028 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_ENABLE_LOCAL_PROGRAMMING_ID | ENABLE LOCAL PROGRAMMING ID | unknown | EnableLocalProgramming attribute |
| 0x0029 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_ENABLE_ONE_TOUCH_LOCKING_ID | ENABLE ONE TOUCH LOCKING ID | unknown | Enable/disable the ability to lock the door lock with a single touch on the door lock. |
| 0x002A | ESP_ZB_ZCL_ATTR_DOOR_LOCK_ENABLE_INSIDE_STATUS_LED_ID | ENABLE INSIDE STATUS LED ID | unknown | Enable/disable an inside LED that allows the user to see at a glance if the door is locked. |
| 0x002B | ESP_ZB_ZCL_ATTR_DOOR_LOCK_ENABLE_PRIVACY_MODE_BUTTON_ID | ENABLE PRIVACY MODE BUTTON ID | unknown | Enable/disable a button inside the door that is used to put the lock into privacy mode. |
| 0x0030 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_WRONG_CODE_ENTRY_LIMIT_ID | WRONG CODE ENTRY LIMIT ID | unknown | The number of incorrect codes or RFID presentment attempts a user is allowed to enter before the door will enter a lockout state. |
| 0x0031 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_USER_CODE_TEMPORARY_DISABLE_TIME_ID | USER CODE TEMPORARY DISABLE TIME ID | unknown | The number of seconds that the lock shuts down following wrong code entry. |
| 0x0032 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_SEND_PIN_OVER_THE_AIR_ID | SEND PIN OVER THE AIR ID | unknown | Boolean set to True if it is ok for the door lock server to send PINs over the air. |
| 0x0033 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_REQUIRE_PIN_RF_ID | REQUIRE PIN RF ID | unknown | Require PIN for RF operation attribute |
| 0x0034 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_SECURITY_LEVEL_ID | SECURITY LEVEL ID | unknown | It allows the door lock manufacturer to indicate what level of security the door lock requires. |
| 0x0040 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_ALARM_MASK_ID | ALARM MASK ID | unknown | The alarm mask is used to turn on/off alarms for particular functions |
| 0x0041 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_KEYPAD_OPERATION_EVENT_MASK_ID | KEYPAD OPERATION EVENT MASK ID | unknown | Event mask used to turn on and off the transmission of keypad operation events. |
| 0x0042 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_RF_OPERATION_EVENT_MASK_ID | RF OPERATION EVENT MASK ID | unknown | Event mask used to turn on and off the transmission of RF operation events. |
| 0x0043 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_MANUAL_OPERATION_EVENT_MASK_ID | MANUAL OPERATION EVENT MASK ID | unknown | Event mask used to turn on and off manual operation events. |
| 0x0044 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_RFID_OPERATION_EVENT_MASK_ID | RFID OPERATION EVENT MASK ID | unknown | Event mask used to turn on and off RFID operation events. |
| 0x0045 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_KEYPAD_PROGRAMMING_EVENT_MASK_ID | KEYPAD PROGRAMMING EVENT MASK ID | unknown | Event mask used to turn on and off keypad programming events. |
| 0x0046 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_RF_PROGRAMMING_EVENT_MASK_ID | RF PROGRAMMING EVENT MASK ID | unknown | Event mask used to turn on and off RF programming events. |
| 0x0047 | ESP_ZB_ZCL_ATTR_DOOR_LOCK_RFID_PROGRAMMING_EVENT_MASK_ID | RFID PROGRAMMING EVENT MASK ID | unknown | Event mask used to turn on and off RFID programming events. |

## 0x0102 Window covering cluster identifier. (WINDOW_COVERING)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_ROLLERSHADE | TYPE ROLLERSHADE | unknown | Rollershade value |
| 0x0000 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_WINDOW_COVERING_TYPE_ID | WINDOW COVERING TYPE ID | unknown | Window Covering Type attribute |
| 0x0001 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CONFIG_OPERATIONAL | CONFIG OPERATIONAL | unknown | Operational value |
| 0x0001 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_PHYSICAL_CLOSED_LIMIT_LIFT_ID | PHYSICAL CLOSED LIMIT LIFT ID | unknown | PhysicalClosedLimit Lift attribute |
| 0x0001 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_REVERSED_MOTOR_DIRECTION | TYPE REVERSED MOTOR DIRECTION | unknown | Reversed motor direction value |
| 0x0001 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_ROLLERSHADE_2_MOTOR | TYPE ROLLERSHADE 2 MOTOR | unknown | Rollershade - 2 Motor value |
| 0x0002 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CONFIG_ONLINE | CONFIG ONLINE | unknown | Online value |
| 0x0002 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_PHY_CLOSED_LIMIT_TILT_ID | PHY CLOSED LIMIT TILT ID | unknown | PhysicalClosedLimit Tilt attribute |
| 0x0002 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_ROLLERSHADE_EXTERIOR | TYPE ROLLERSHADE EXTERIOR | unknown | Rollershade - Exterior value |
| 0x0002 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_RUN_IN_CALIBRATION_MODE | TYPE RUN IN CALIBRATION MODE | unknown | Run in calibration mode value |
| 0x0003 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_LIFT_ID | CURRENT POSITION LIFT ID | unknown | CurrentPosition Lift attribute |
| 0x0003 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_ROLLERSHADE_EXTERIOR_2_MOTOR | TYPE ROLLERSHADE EXTERIOR 2 MOTOR | unknown | Rollershade - Exterior - 2 Motor value |
| 0x0004 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CONFIG_REVERSE_COMMANDS | CONFIG REVERSE COMMANDS | unknown | Open/Up Commands have been reversed value |
| 0x0004 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_TILT_ID | CURRENT POSITION TILT ID | unknown | CurrentPosition Tilt attribute |
| 0x0004 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_DRAPERY | TYPE DRAPERY | unknown | Drapery value |
| 0x0004 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_MOTOR_IS_RUNNING_IN_MAINTENANCE_MODE | TYPE MOTOR IS RUNNING IN MAINTENANCE MODE | unknown | Motor is running in maintenance mode value |
| 0x0005 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_NUMBER_OF_ACTUATIONS_LIFT_ID | NUMBER OF ACTUATIONS LIFT ID | unknown | Number of Actuations Lift attribute |
| 0x0005 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_AWNING | TYPE AWNING | unknown | Awning value |
| 0x0006 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_NUMBER_OF_ACTUATIONS_TILT_ID | NUMBER OF ACTUATIONS TILT ID | unknown | Number of Actuations Tilt attribute |
| 0x0006 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_SHUTTER | TYPE SHUTTER | unknown | Shutter value |
| 0x0007 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CONFIG_STATUS_ID | CONFIG STATUS ID | unknown | Config/Status attribute |
| 0x0007 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_TILT_BLIND_TILT_ONLY | TYPE TILT BLIND TILT ONLY | unknown | Tilt Blind - Tilt Only value |
| 0x0008 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CONFIG_LIFT_CONTROL_IS_CLOSED_LOOP | CONFIG LIFT CONTROL IS CLOSED LOOP | unknown | Lift control is Closed Loop value |
| 0x0008 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_LIFT_PERCENTAGE_ID | CURRENT POSITION LIFT PERCENTAGE ID | unknown | Current Position Lift Percentage attribute |
| 0x0008 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_LEDS_WILL_DISPLAY_FEEDBACK | TYPE LEDS WILL DISPLAY FEEDBACK | unknown | LEDs will display feedback value |
| 0x0008 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_TILT_BLIND_LIFT_AND_TILT | TYPE TILT BLIND LIFT AND TILT | unknown | Tilt Blind - Lift and Tilt value |
| 0x0009 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CURRENT_POSITION_TILT_PERCENTAGE_ID | CURRENT POSITION TILT PERCENTAGE ID | unknown | Current Position Tilt Percentage attribute |
| 0x0009 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_TYPE_PROJECTOR_SCREEN | TYPE PROJECTOR SCREEN | unknown | Projector screen value |
| 0x0010 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CONFIG_TILT_CONTROL_IS_CLOSED_LOOP | CONFIG TILT CONTROL IS CLOSED LOOP | unknown | Tilt control is Closed Loop value |
| 0x0010 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_INSTALLED_OPEN_LIMIT_LIFT_ID | INSTALLED OPEN LIMIT LIFT ID | unknown | InstalledOpenLimit -  Lift attribute |
| 0x0011 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_INSTALLED_CLOSED_LIMIT_LIFT_ID | INSTALLED CLOSED LIMIT LIFT ID | unknown | InstalledClosedLimit - Lift attribute |
| 0x0012 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_INSTALLED_OPEN_LIMIT_TILT_ID | INSTALLED OPEN LIMIT TILT ID | unknown | InstalledOpenLimit - Tilt attribute |
| 0x0013 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_INSTALLED_CLOSED_LIMIT_TILT_ID | INSTALLED CLOSED LIMIT TILT ID | unknown | InstalledClosedLimit - Tilt attribute |
| 0x0014 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_VELOCITY_ID | VELOCITY ID | unknown | Velocity - Lift attribute |
| 0x0015 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_ACCELERATION_TIME_ID | ACCELERATION TIME ID | unknown | Acceleration Time - Lift attribute |
| 0x0016 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_DECELERATION_TIME_ID | DECELERATION TIME ID | unknown | Deceleration Time - Lift attribute |
| 0x0017 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_MODE_ID | MODE ID | unknown | Mode attribute |
| 0x0018 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_INTERMEDIATE_SETPOINTS_LIFT_ID | INTERMEDIATE SETPOINTS LIFT ID | unknown | Intermediate Setpoints - Lift attribute |
| 0x0019 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_INTERMEDIATE_SETPOINTS_TILT_ID | INTERMEDIATE SETPOINTS TILT ID | unknown | Intermediate Setpoints - Tilt attribute |
| 0x0020 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CONFIG_LIFT_ENCODER_CONTROLLED | CONFIG LIFT ENCODER CONTROLLED | unknown | Lift Encoder Controlled value |
| 0x0040 | ESP_ZB_ZCL_ATTR_WINDOW_COVERING_CONFIG_TILT_ENCODER_CONTROLLED | CONFIG TILT ENCODER CONTROLLED | unknown | Tilt Encoder Controlled value |

## 0x0201 Thermostat cluster identifier. (THERMOSTAT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_ID | LOCAL TEMPERATURE ID | unknown | Local Temperature attribute |
| 0x0001 | ESP_ZB_ZCL_ATTR_THERMOSTAT_OUTDOOR_TEMPERATURE_ID | OUTDOOR TEMPERATURE ID | unknown | Outdoor Temperature attribute |
| 0x0002 | ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPANCY_ID | OCCUPANCY ID | unknown | Occupancy attribute |
| 0x0003 | ESP_ZB_ZCL_ATTR_THERMOSTAT_ABS_MIN_HEAT_SETPOINT_LIMIT_ID | ABS MIN HEAT SETPOINT LIMIT ID | unknown | The AbsMinHeatSetpointLimit attribute specifies the absolute minimum level that the heating setpoint MAY be set to |
| 0x0004 | ESP_ZB_ZCL_ATTR_THERMOSTAT_ABS_MAX_HEAT_SETPOINT_LIMIT_ID | ABS MAX HEAT SETPOINT LIMIT ID | unknown | The AbsMaxHeatSetpointLimit attribute specifies the absolute maximum level that the heating setpoint MAY be set to |
| 0x0005 | ESP_ZB_ZCL_ATTR_THERMOSTAT_ABS_MIN_COOL_SETPOINT_LIMIT_ID | ABS MIN COOL SETPOINT LIMIT ID | unknown | The AbsMinCoolSetpointLimit attribute specifies the absolute minimum level that the cooling setpoint MAY be set to |
| 0x0006 | ESP_ZB_ZCL_ATTR_THERMOSTAT_ABS_MAX_COOL_SETPOINT_LIMIT_ID | ABS MAX COOL SETPOINT LIMIT ID | unknown | The AbsMaxCoolSetpointLimit attribute specifies the absolute maximum level that the cooling setpoint MAY be set to |
| 0x0007 | ESP_ZB_ZCL_ATTR_THERMOSTAT_PI_COOLING_DEMAND_ID | PI COOLING DEMAND ID | unknown | The PICoolingDemand attribute is 8 bits in length and specifies the level of cooling demanded by the PI (proportional integral) control loop in use by the thermostat (if any), in percent |
| 0x0008 | ESP_ZB_ZCL_ATTR_THERMOSTAT_PI_HEATING_DEMAND_ID | PI HEATING DEMAND ID | unknown | The PIHeatingDemand attribute is 8 bits in length and specifies the level of heating demanded by the PI loop in percent |
| 0x0009 | ESP_ZB_ZCL_ATTR_THERMOSTAT_HVAC_SYSTEM_TYPE_CONFIGURATION_ID | HVAC SYSTEM TYPE CONFIGURATION ID | unknown | The HVACSystemTypeConfiguration attribute specifies the HVAC system type controlled by the thermostat |
| 0x0010 | ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_CALIBRATION_ID | LOCAL TEMPERATURE CALIBRATION ID | unknown | Local Temperature Calibration |
| 0x0011 | ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID | OCCUPIED COOLING SETPOINT ID | unknown | Occupied Cooling Setpoint attribute |
| 0x0012 | ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID | OCCUPIED HEATING SETPOINT ID | unknown | Occupied Heating Setpoint attribute |
| 0x0013 | ESP_ZB_ZCL_ATTR_THERMOSTAT_UNOCCUPIED_COOLING_SETPOINT_ID | UNOCCUPIED COOLING SETPOINT ID | unknown | Unoccupied Cooling Setpoint attribute |
| 0x0014 | ESP_ZB_ZCL_ATTR_THERMOSTAT_UNOCCUPIED_HEATING_SETPOINT_ID | UNOCCUPIED HEATING SETPOINT ID | unknown | Unoccupied Heating Setpoint attribute |
| 0x0015 | ESP_ZB_ZCL_ATTR_THERMOSTAT_MIN_HEAT_SETPOINT_LIMIT_ID | MIN HEAT SETPOINT LIMIT ID | unknown | The MinHeatSetpointLimit attribute specifies the minimum level that the heating setpoint MAY be set to |
| 0x0016 | ESP_ZB_ZCL_ATTR_THERMOSTAT_MAX_HEAT_SETPOINT_LIMIT_ID | MAX HEAT SETPOINT LIMIT ID | unknown | The MaxHeatSetpointLimit attribute specifies the maximum level that the heating setpoint MAY be set to |
| 0x0017 | ESP_ZB_ZCL_ATTR_THERMOSTAT_MIN_COOL_SETPOINT_LIMIT_ID | MIN COOL SETPOINT LIMIT ID | unknown | The MinCoolSetpointLimit attribute specifies the minimum level that the cooling setpoint MAY be set to |
| 0x0018 | ESP_ZB_ZCL_ATTR_THERMOSTAT_MAX_COOL_SETPOINT_LIMIT_ID | MAX COOL SETPOINT LIMIT ID | unknown | The MaxCoolSetpointLimit attribute specifies the maximum level that the cooling setpoint MAY be set to |
| 0x0019 | ESP_ZB_ZCL_ATTR_THERMOSTAT_MIN_SETPOINT_DEAD_BAND_ID | MIN SETPOINT DEAD BAND ID | unknown | The MinSetpointDeadBand attribute specifies the minimum difference between the Heat Setpoint and the Cool SetPoint, in steps of 0.1C |
| 0x001A | ESP_ZB_ZCL_ATTR_THERMOSTAT_REMOTE_SENSING_ID | REMOTE SENSING ID | unknown | The RemoteSensing attribute is an 8-bit bitmap that specifies whether the local temperature, outdoor temperature and occupancy are being sensed by internal sensors or remote networked sensors |
| 0x001B | ESP_ZB_ZCL_ATTR_THERMOSTAT_CONTROL_SEQUENCE_OF_OPERATION_ID | CONTROL SEQUENCE OF OPERATION ID | unknown | Control Sequence Of Operation attribute |
| 0x001C | ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID | SYSTEM MODE ID | unknown | System Mode attribute |
| 0x001D | ESP_ZB_ZCL_ATTR_THERMOSTAT_ALARM_MASK_ID | ALARM MASK ID | unknown | The AlarmMask attribute specifies whether each of the alarms is enabled |
| 0x001E | ESP_ZB_ZCL_ATTR_THERMOSTAT_RUNNING_MODE_ID | RUNNING MODE ID | unknown | Thermostat Running Mode attribute |
| 0x0020 | ESP_ZB_ZCL_ATTR_THERMOSTAT_START_OF_WEEK_ID | START OF WEEK ID | unknown | Start of Week attribute |
| 0x0021 | ESP_ZB_ZCL_ATTR_THERMOSTAT_NUMBER_OF_WEEKLY_TRANSITIONS_ID | NUMBER OF WEEKLY TRANSITIONS ID | unknown | NumberOfWeeklyTransitions attribute determines how many weekly schedule transitions the thermostat is capable of handling |
| 0x0022 | ESP_ZB_ZCL_ATTR_THERMOSTAT_NUMBER_OF_DAILY_TRANSITIONS_ID | NUMBER OF DAILY TRANSITIONS ID | unknown | NumberOfDailyTransitions attribute determines how many daily schedule transitions the thermostat is capable of handling |
| 0x0023 | ESP_ZB_ZCL_ATTR_THERMOSTAT_TEMPERATURE_SETPOINT_HOLD_ID | TEMPERATURE SETPOINT HOLD ID | unknown | TemperatureSetpointHold specifies the temperature hold status on the thermostat |
| 0x0024 | ESP_ZB_ZCL_ATTR_THERMOSTAT_TEMPERATURE_SETPOINT_HOLD_DURATION_ID | TEMPERATURE SETPOINT HOLD DURATION ID | unknown | TemperatureSetpointHoldDuration sets the period in minutes for which a setpoint hold is active |
| 0x0025 | ESP_ZB_ZCL_ATTR_THERMOSTAT_THERMOSTAT_PROGRAMMING_OPERATION_MODE_ID | THERMOSTAT PROGRAMMING OPERATION MODE ID | unknown | The ThermostatProgrammingOperationMode attribute determines the operational state of the thermostats programming |
| 0x0029 | ESP_ZB_ZCL_ATTR_THERMOSTAT_THERMOSTAT_RUNNING_STATE_ID | THERMOSTAT RUNNING STATE ID | unknown | ThermostatRunningState represents the current relay state of the heat, cool, and fan relays |
| 0x0030 | ESP_ZB_ZCL_ATTR_THERMOSTAT_SETPOINT_CHANGE_SOURCE_ID | SETPOINT CHANGE SOURCE ID | unknown | The SetpointChangeSource attribute specifies the source of the current active OccupiedCoolingSetpoint or OccupiedHeatingSetpoint (i.e., who or what determined the current setpoint) |
| 0x0031 | ESP_ZB_ZCL_ATTR_THERMOSTAT_SETPOINT_CHANGE_AMOUNT_ID | SETPOINT CHANGE AMOUNT ID | unknown | The SetpointChangeAmount attribute specifies the delta between the current active OccupiedCoolingSetpoint or OccupiedHeatingSetpoint and the previous active setpoint |
| 0x0032 | ESP_ZB_ZCL_ATTR_THERMOSTAT_SETPOINT_CHANGE_SOURCE_TIMESTAMP_ID | SETPOINT CHANGE SOURCE TIMESTAMP ID | unknown | The SetpointChangeSourceTimestamp attribute specifies the time in UTC at which the SetpointChangeSourceAmount attribute change was recorded |
| 0x0034 | ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_SETBACK_ID | OCCUPIED SETBACK ID | unknown | Specifies the degrees Celsius, in 0.1 degree increments, the Thermostat server will allow the LocalTemperature attribute to float above the OccupiedCooling setpoint or below the OccupiedHeating setpoint before initiating a state change to bring the temperature back to the users desired setpoint |
| 0x0035 | ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_SETBACK_MIN_ID | OCCUPIED SETBACK MIN ID | unknown | Specifies the minimum degrees Celsius, in 0.1 degree increments, the Thermostat server will allow the OccupiedSetback attribute  to be configured by a user |
| 0x0036 | ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_SETBACK_MAX_ID | OCCUPIED SETBACK MAX ID | unknown | Specifies the maximum degrees Celsius, in 0.1 degree increments, the Thermostat server will allow the OccupiedSetback attribute to be configured by a user |
| 0x0037 | ESP_ZB_ZCL_ATTR_THERMOSTAT_UNOCCUPIED_SETBACK_ID | UNOCCUPIED SETBACK ID | unknown | Specifies the degrees Celsius, in 0.1 degree increments, the Thermostat server will allow the LocalTemperature attribute to float above the UnoccupiedCooling setpoint or below the UnoccupiedHeating setpoint before initiating a state change to bring the temperature back to the users desired setpoint |
| 0x0038 | ESP_ZB_ZCL_ATTR_THERMOSTAT_UNOCCUPIED_SETBACK_MIN_ID | UNOCCUPIED SETBACK MIN ID | unknown | Specifies the minimum degrees Celsius, in 0.1 degree increments, the Thermostat server will allow the UnoccupiedSetback attribute to be configured by a user |
| 0x0039 | ESP_ZB_ZCL_ATTR_THERMOSTAT_UNOCCUPIED_SETBACK_MAX_ID | UNOCCUPIED SETBACK MAX ID | unknown | Specifies the maximum degrees Celsius, in 0.1 degree increments, the Thermostat server will allow the UnoccupiedSetback attribute to be configured by a user. |
| 0x003A | ESP_ZB_ZCL_ATTR_THERMOSTAT_EMERGENCY_HEAT_DELTA_ID | EMERGENCY HEAT DELTA ID | unknown | Specifies the delta, in 0.1 degrees Celsius, between LocalTemperature and the OccupiedHeatingSetpoint or UnoccupiedHeatingSetpoint attributes at which the Thermostat server will operate in emergency heat mode |
| 0x0040 | ESP_ZB_ZCL_ATTR_THERMOSTAT_AC_TYPE_ID | AC TYPE ID | unknown | Indicates the type of Mini Split ACType of Mini Split AC is defined depending on how Cooling and Heating condition is achieved by Mini Split AC |
| 0x0041 | ESP_ZB_ZCL_ATTR_THERMOSTAT_AC_CAPACITY_ID | AC CAPACITY ID | unknown | Indicates capacity of Mini Split AC in terms of the format defined by the ACCapacityFormat attribute |
| 0x0042 | ESP_ZB_ZCL_ATTR_THERMOSTAT_AC_REFRIGERANT_TYPE_ID | AC REFRIGERANT TYPE ID | unknown | Indicates type of refrigerant used within the Mini Split AC |
| 0x0043 | ESP_ZB_ZCL_ATTR_THERMOSTAT_AC_COMPRESSOR_TYPE_ID | AC COMPRESSOR TYPE ID | unknown | This indicates type of Compressor used within the Mini Split AC |
| 0x0044 | ESP_ZB_ZCL_ATTR_THERMOSTAT_AC_ERROR_CODE_ID | AC ERROR CODE ID | unknown | This indicates the type of errors encountered within the Mini Split AC |
| 0x0045 | ESP_ZB_ZCL_ATTR_THERMOSTAT_AC_LOUVER_POSITION_ID | AC LOUVER POSITION ID | unknown | AC Louver position attribute |
| 0x0046 | ESP_ZB_ZCL_ATTR_THERMOSTAT_AC_COIL_TEMPERATURE_ID | AC COIL TEMPERATURE ID | unknown | ACCoilTemperature represents the temperature in degrees Celsius, as measured locally or remotely (over the network) |
| 0x0047 | ESP_ZB_ZCL_ATTR_THERMOSTAT_AC_CAPACITY_FORMAT_ID | AC CAPACITY FORMAT ID | unknown | This is the format for the ACCapacity attribute |

## 0x0202 Fan control cluster identifier. (FAN_CONTROL)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID | FAN MODE ID | unknown | Fan mode attribute |
| 0x0001 | ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_SEQUENCE_ID | FAN MODE SEQUENCE ID | unknown | Fan mode sequence attribute |

## 0x0203 Dehumidification control cluster identifier. (DEHUMIDIFICATION_CONTROL)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_DEHUMIDIFICATION_CONTROL_RELATIVE_HUMIDITY_ID | RELATIVE HUMIDITY ID | unknown |  |
| 0x0001 | ESP_ZB_ZCL_ATTR_DEHUMIDIFICATION_CONTROL_DEHUMIDIFICATION_COOLING_ID | DEHUMIDIFICATION COOLING ID | unknown | Dehumidification Cooling attribute |
| 0x0010 | ESP_ZB_ZCL_ATTR_DEHUMIDIFICATION_CONTROL_RHDEHUMIDIFICATION_SETPOINT_ID | RHDEHUMIDIFICATION SETPOINT ID | unknown | RHDehumidification Setpoint attribute |
| 0x0011 | ESP_ZB_ZCL_ATTR_DEHUMIDIFICATION_CONTROL_RELATIVE_HUMIDITY_MODE_ID | RELATIVE HUMIDITY MODE ID | unknown |  |
| 0x0012 | ESP_ZB_ZCL_ATTR_DEHUMIDIFICATION_CONTROL_DEHUMIDIFICATION_LOCKOUT_ID | DEHUMIDIFICATION LOCKOUT ID | unknown |  |
| 0x0013 | ESP_ZB_ZCL_ATTR_DEHUMIDIFICATION_CONTROL_DEHUMIDIFICATION_HYSTERESIS_ID | DEHUMIDIFICATION HYSTERESIS ID | unknown | Dehumidification Hysteresis attribute |
| 0x0014 | ESP_ZB_ZCL_ATTR_DEHUMIDIFICATION_CONTROL_DEHUMIDIFICATION_MAX_COOL_ID | DEHUMIDIFICATION MAX COOL ID | unknown | Dehumidification Max Cool attribute |
| 0x0015 | ESP_ZB_ZCL_ATTR_DEHUMIDIFICATION_CONTROL_RELATIVE_HUMIDITY_DISPLAY_ID | RELATIVE HUMIDITY DISPLAY ID | unknown |  |

## 0x0204 Thermostat user interface configuration cluster identifier. (THERMOSTAT_UI_CONFIG)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_THERMOSTAT_UI_CONFIG_TEMPERATURE_DISPLAY_MODE_ID | TEMPERATURE DISPLAY MODE ID | unknown | Temperature Display Mode attribute |
| 0x0001 | ESP_ZB_ZCL_ATTR_THERMOSTAT_UI_CONFIG_KEYPAD_LOCKOUT_ID | KEYPAD LOCKOUT ID | unknown | Keypad Lockout attribute |
| 0x0002 | ESP_ZB_ZCL_ATTR_THERMOSTAT_UI_CONFIG_SCHEDULE_PROGRAMMING_VISIBILITY_ID | SCHEDULE PROGRAMMING VISIBILITY ID | unknown | The Schedule ProgrammingVisibility attribute is used to hide the weekly schedule programming functionality or menu on a thermostat from a user to prevent local user programming of the weekly schedule. |

## 0x0300 Color control cluster identifier. (COLOR_CONTROL)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID | CURRENT HUE ID | unknown | Current_HUE attribute |
| 0x0001 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID | CURRENT SATURATION ID | unknown | Current Saturation attribute |
| 0x0002 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_REMAINING_TIME_ID | REMAINING TIME ID | unknown | Remaining Time attribute |
| 0x0003 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID | CURRENT X ID | unknown | CurrentX attribute |
| 0x0004 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID | CURRENT Y ID | unknown | CurrentY attribute |
| 0x0005 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_DRIFT_COMPENSATION_ID | DRIFT COMPENSATION ID | unknown | The DriftCompensation attribute indicates what mechanism |
| 0x0006 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COMPENSATION_TEXT_ID | COMPENSATION TEXT ID | unknown | The CompensationText attribute holds a textual indication of what mechanism |
| 0x0007 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID | COLOR TEMPERATURE ID | unknown | Color Temperature Mireds attribute |
| 0x0008 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID | COLOR MODE ID | unknown | Color Mode attribute |
| 0x000F | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_OPTIONS_ID | OPTIONS ID | unknown | The Options attribute is a bitmap that determines the default behavior of some cluster commands. |
| 0x4000 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_CURRENT_HUE_ID | ENHANCED CURRENT HUE ID | unknown | The EnhancedCurrentHue attribute represents non-equidistant steps along the CIE 1931 color triangle. |
| 0x4001 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_COLOR_MODE_ID | ENHANCED COLOR MODE ID | unknown | The EnhancedColorMode attribute specifies which attributes are currently determining the color of the device. |
| 0x4002 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_ACTIVE_ID | COLOR LOOP ACTIVE ID | unknown | The ColorLoopActive attribute specifies the current active status of the color loop. |
| 0x4003 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_DIRECTION_ID | COLOR LOOP DIRECTION ID | unknown | The ColorLoopDirection attribute specifies the current direction of the color loop. |
| 0x4004 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_TIME_ID | COLOR LOOP TIME ID | unknown | The ColorLoopTime attribute specifies the number of seconds it SHALL take to perform a full color loop. |
| 0x4005 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_START_ENHANCED_HUE_ID | COLOR LOOP START ENHANCED HUE ID | unknown | The ColorLoopStartEnhancedHue attribute specifies the value of the EnhancedCurrentHue attribute from which the color loop SHALL be started. |
| 0x4006 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_LOOP_STORED_ENHANCED_HUE_ID | COLOR LOOP STORED ENHANCED HUE ID | unknown | The ColorLoopStoredEnhancedHue attribute specifies the value of the EnhancedCurrentHue attribute before the color loop was stored. |
| 0x400A | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_CAPABILITIES_ID | COLOR CAPABILITIES ID | unknown | The ColorCapabilities attribute specifies the color capabilities of the device |
| 0x400B | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID | COLOR TEMP PHYSICAL MIN MIREDS ID | unknown | The ColorTempPhysicalMinMireds attribute indicates the minimum mired value supported by the hardware. |
| 0x400C | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID | COLOR TEMP PHYSICAL MAX MIREDS ID | unknown | The ColorTempPhysicalMaxMireds attribute indicates the maximum mired value supported by the hardware. |
| 0x400D | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COUPLE_COLOR_TEMP_TO_LEVEL_MIN_MIREDS_ID | COUPLE COLOR TEMP TO LEVEL MIN MIREDS ID | unknown | The CoupleColorTempToLevelMinMireds attribute specifies a lower bound on the value of the ColorTemperatureMireds attribute |
| 0x4010 | ESP_ZB_ZCL_ATTR_COLOR_CONTROL_START_UP_COLOR_TEMPERATURE_MIREDS_ID | START UP COLOR TEMPERATURE MIREDS ID | unknown | The StartUpColorTemperatureMireds attribute SHALL define the desired startup color temperature value a lamp SHALL use when it is supplied with power. |

## 0x0400 Illuminance measurement (ILLUMINANCE_MEASUREMENT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID | MEASURED VALUE ID | unknown | MeasuredValue |
| 0x0001 | ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MIN_MEASURED_VALUE_ID | MIN MEASURED VALUE ID | unknown | MinMeasuredValue Attribute |
| 0x0002 | ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MAX_MEASURED_VALUE_ID | MAX MEASURED VALUE ID | unknown | MaxMeasuredValue Attribute |
| 0x0003 | ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_TOLERANCE_ID | TOLERANCE ID | unknown | The Tolerance attribute SHALL indicate the magnitude of the possible error that is associated with MeasuredValue, using the same units and resolution. |
| 0x0004 | ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_LIGHT_SENSOR_TYPE_ID | LIGHT SENSOR TYPE ID | unknown | The LightSensorType attribute specifies the electronic type of the light sensor. |

## 0x0402 Temperature measurement (TEMP_MEASUREMENT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID | VALUE ID | unknown | MeasuredValue |
| 0x0001 | ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_MIN_VALUE_ID | MIN VALUE ID | unknown | MinMeasuredValue |
| 0x0002 | ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_MAX_VALUE_ID | MAX VALUE ID | unknown | MaxMeasuredValue |
| 0x0003 | ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_TOLERANCE_ID | TOLERANCE ID | unknown | Tolerance |

## 0x0403 Pressure measurement (PRESSURE_MEASUREMENT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_VALUE_ID | VALUE ID | unknown | MeasuredValue |
| 0x0001 | ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_MIN_VALUE_ID | MIN VALUE ID | unknown | MinMeasuredValue |
| 0x0002 | ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_MAX_VALUE_ID | MAX VALUE ID | unknown | MaxMeasuredValue |
| 0x0003 | ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_TOLERANCE_ID | TOLERANCE ID | unknown | MeasuredTolerance |
| 0x0010 | ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_SCALED_VALUE_ID | SCALED VALUE ID | unknown | ScaledValue |
| 0x0011 | ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_MIN_SCALED_VALUE_ID | MIN SCALED VALUE ID | unknown | MinScaledValue |
| 0x0012 | ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_MAX_SCALED_VALUE_ID | MAX SCALED VALUE ID | unknown | MaxScaledValue |
| 0x0013 | ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_SCALED_TOLERANCE_ID | SCALED TOLERANCE ID | unknown | ScaledTolerance |
| 0x0014 | ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_SCALE_ID | SCALE ID | unknown | Scale |

## 0x0404 Flow measurement (FLOW_MEASUREMENT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_FLOW_MEASUREMENT_VALUE_ID | VALUE ID | unknown | MeasuredValue attribute |
| 0x0001 | ESP_ZB_ZCL_ATTR_FLOW_MEASUREMENT_MIN_VALUE_ID | MIN VALUE ID | unknown | MinMeasuredValue attribute |
| 0x0002 | ESP_ZB_ZCL_ATTR_FLOW_MEASUREMENT_MAX_VALUE_ID | MAX VALUE ID | unknown | MaxMeasuredValue attribute |
| 0x0003 | ESP_ZB_ZCL_ATTR_FLOW_MEASUREMENT_TOLERANCE_ID | TOLERANCE ID | unknown | Tolerance attribute |

## 0x0405 Relative humidity measurement (REL_HUMIDITY_MEASUREMENT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID | VALUE ID | unknown | MeasuredValue |
| 0x0001 | ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_MIN_VALUE_ID | MIN VALUE ID | unknown | MinMeasuredValue Attribute |
| 0x0002 | ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_MAX_VALUE_ID | MAX VALUE ID | unknown | MaxMeasuredValue Attribute |

## 0x0406 Occupancy sensing (OCCUPANCY_SENSING)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID | OCCUPANCY ID | unknown | Occupancy attribute identifier |
| 0x0001 | ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_ID | OCCUPANCY SENSOR TYPE ID | unknown | Occupancy Sensor Type attribute identifier |
| 0x0002 | ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_BITMAP_ID | OCCUPANCY SENSOR TYPE BITMAP ID | unknown | The OccupancySensorTypeBitmap attribute specifies the types of the occupancy sensor. |
| 0x0010 | ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_PIR_OCC_TO_UNOCC_DELAY_ID | PIR OCC TO UNOCC DELAY ID | unknown | PIROccupiedToUnoccupiedDelay identifier |
| 0x0011 | ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_PIR_UNOCC_TO_OCC_DELAY_ID | PIR UNOCC TO OCC DELAY ID | unknown | PIRUnoccupiedToOccupiedDelay identifier |
| 0x0012 | ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_PIR_UNOCC_TO_OCC_THRESHOLD_ID | PIR UNOCC TO OCC THRESHOLD ID | unknown | PIRUnoccupiedToOccupiedThreshold identifier |
| 0x0020 | ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_ULTRASONIC_OCCUPIED_TO_UNOCCUPIED_DELAY_ID | ULTRASONIC OCCUPIED TO UNOCCUPIED DELAY ID | unknown |  |
| 0x0022 | ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_ULTRASONIC_UNOCCUPIED_TO_OCCUPIED_THRESHOLD_ID | ULTRASONIC UNOCCUPIED TO OCCUPIED THRESHOLD ID | unknown |  |
| 0x0031 | ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_PHYSICAL_CONTACT_UNOCCUPIED_TO_OCCUPIED_DELAY_ID | PHYSICAL CONTACT UNOCCUPIED TO OCCUPIED DELAY ID | unknown |  |

## 0x0409 pH measurement (PH_MEASUREMENT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_PH_MEASUREMENT_MEASURED_VALUE_ID | MEASURED VALUE ID | unknown |  |
| 0x0001 | ESP_ZB_ZCL_ATTR_PH_MEASUREMENT_MIN_MEASURED_VALUE_ID | MIN MEASURED VALUE ID | unknown |  |
| 0x0002 | ESP_ZB_ZCL_ATTR_PH_MEASUREMENT_MAX_MEASURED_VALUE_ID | MAX MEASURED VALUE ID | unknown |  |
| 0x0003 | ESP_ZB_ZCL_ATTR_PH_MEASUREMENT_TOLERANCE_ID | TOLERANCE ID | unknown |  |

## 0x040A Electrical conductivity measurement (EC_MEASUREMENT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_EC_MEASUREMENT_MEASURED_VALUE_ID | MEASURED VALUE ID | unknown |  |
| 0x0001 | ESP_ZB_ZCL_ATTR_EC_MEASUREMENT_MIN_MEASURED_VALUE_ID | MIN MEASURED VALUE ID | unknown |  |
| 0x0002 | ESP_ZB_ZCL_ATTR_EC_MEASUREMENT_MAX_MEASURED_VALUE_ID | MAX MEASURED VALUE ID | unknown |  |
| 0x0003 | ESP_ZB_ZCL_ATTR_EC_MEASUREMENT_TOLERANCE_ID | TOLERANCE ID | unknown |  |

## 0x040B Wind speed measurement (WIND_SPEED_MEASUREMENT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_WIND_SPEED_MEASUREMENT_MEASURED_VALUE_ID | MEASURED VALUE ID | unknown |  |
| 0x0001 | ESP_ZB_ZCL_ATTR_WIND_SPEED_MEASUREMENT_MIN_MEASURED_VALUE_ID | MIN MEASURED VALUE ID | unknown |  |
| 0x0002 | ESP_ZB_ZCL_ATTR_WIND_SPEED_MEASUREMENT_MAX_MEASURED_VALUE_ID | MAX MEASURED VALUE ID | unknown |  |
| 0x0003 | ESP_ZB_ZCL_ATTR_WIND_SPEED_MEASUREMENT_TOLERANCE_ID | TOLERANCE ID | unknown |  |

## 0x040D Carbon dioxide measurement (CARBON_DIOXIDE_MEASUREMENT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_CARBON_DIOXIDE_MEASUREMENT_MEASURED_VALUE_ID | MEASURED VALUE ID | unknown | MeasuredValue attribute |
| 0x0001 | ESP_ZB_ZCL_ATTR_CARBON_DIOXIDE_MEASUREMENT_MIN_MEASURED_VALUE_ID | MIN MEASURED VALUE ID | unknown | MinMeasuredValue attribute |
| 0x0002 | ESP_ZB_ZCL_ATTR_CARBON_DIOXIDE_MEASUREMENT_MAX_MEASURED_VALUE_ID | MAX MEASURED VALUE ID | unknown | MaxMeasuredValue attribute |
| 0x0003 | ESP_ZB_ZCL_ATTR_CARBON_DIOXIDE_MEASUREMENT_TOLERANCE_ID | TOLERANCE ID | unknown | Tolerance attribute |

## 0x042A PM2.5 measurement (PM2_5_MEASUREMENT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_PM2_5_MEASUREMENT_MEASURED_VALUE_ID | MEASURED VALUE ID | unknown | MeasuredValue attribute |
| 0x0001 | ESP_ZB_ZCL_ATTR_PM2_5_MEASUREMENT_MIN_MEASURED_VALUE_ID | MIN MEASURED VALUE ID | unknown | MinMeasuredValue attribute |
| 0x0002 | ESP_ZB_ZCL_ATTR_PM2_5_MEASUREMENT_MAX_MEASURED_VALUE_ID | MAX MEASURED VALUE ID | unknown | MaxMeasuredValue attribute |
| 0x0003 | ESP_ZB_ZCL_ATTR_PM2_5_MEASUREMENT_TOLERANCE_ID | TOLERANCE ID | unknown | Tolerance attribute |

## 0x0500 IAS zone (IAS_ZONE)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONESTATE_ID | ZONESTATE ID | unknown | ZoneState attribute |
| 0x0001 | ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONETYPE_ID | ZONETYPE ID | unknown | ZoneType attribute |
| 0x0002 | ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONESTATUS_ID | ZONESTATUS ID | unknown | ZoneStatus attribute |
| 0x0010 | ESP_ZB_ZCL_ATTR_IAS_ZONE_IAS_CIE_ADDRESS_ID | IAS CIE ADDRESS ID | unknown | IAS_CIE_Address attribute |
| 0x0011 | ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONEID_ID | ZONEID ID | unknown | ZoneID attribute |
| 0x0012 | ESP_ZB_ZCL_ATTR_IAS_ZONE_NUMBER_OF_ZONE_SENSITIVITY_LEVELS_SUPPORTED_ID | NUMBER OF ZONE SENSITIVITY LEVELS SUPPORTED ID | unknown | NumberOfZoneSensitivityLevelsSupported attribute |
| 0x0013 | ESP_ZB_ZCL_ATTR_IAS_ZONE_CURRENT_ZONE_SENSITIVITY_LEVEL_ID | CURRENT ZONE SENSITIVITY LEVEL ID | unknown | CurrentZoneSensitivityLevel attribute |
| 0xEFFE | ESP_ZB_ZCL_ATTR_IAS_ZONE_INT_CTX_ID | INT CTX ID | unknown | Application context |

## 0x0502 IAS WD (IAS_WD)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_IAS_WD_MAX_DURATION_ID | MAX DURATION ID | unknown |  |

## 0x0700 Price cluster identifier. (PRICE)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_PRICE_CLI_PRICE_INCREASE_RANDOMIZE_MINUTES | CLI PRICE INCREASE RANDOMIZE MINUTES | unknown |  |
| 0x0000 | ESP_ZB_ZCL_ATTR_PRICE_TARIFF_RESOLUTION_PERIOD_NOT_DEFINED | TARIFF RESOLUTION PERIOD NOT DEFINED | unknown |  |
| 0x0001 | ESP_ZB_ZCL_ATTR_PRICE_CLI_PRICE_DECREASE_RANDOMIZE_MINUTES | CLI PRICE DECREASE RANDOMIZE MINUTES | unknown |  |
| 0x0001 | ESP_ZB_ZCL_ATTR_PRICE_CO2_UNIT_KG_PER_KWH | CO2 UNIT KG PER KWH | unknown |  |
| 0x0002 | ESP_ZB_ZCL_ATTR_PRICE_CLI_COMMODITY_TYPE | CLI COMMODITY TYPE | unknown |  |
| 0x0002 | ESP_ZB_ZCL_ATTR_PRICE_CO2_UNIT_KG_PER_GALLON_OF_GASOLINE | CO2 UNIT KG PER GALLON OF GASOLINE | unknown |  |
| 0x0003 | ESP_ZB_ZCL_ATTR_PRICE_CO2_UNIT_KG_PER_THERM_OF_NATURAL_GAS | CO2 UNIT KG PER THERM OF NATURAL GAS | unknown |  |

## 0x0701 Demand Response and Load Control cluster identifier (DRLC)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_DRLC_UTILITY_ENROLLMENT_GROUP | UTILITY ENROLLMENT GROUP | unknown |  |
| 0x0001 | ESP_ZB_ZCL_ATTR_DRLC_START_RANDOMIZATION_MINUTES | START RANDOMIZATION MINUTES | unknown |  |
| 0x0002 | ESP_ZB_ZCL_ATTR_DRLC_DURATION_RANDOMIZATION_MINUTES | DURATION RANDOMIZATION MINUTES | unknown |  |
| 0x0003 | ESP_ZB_ZCL_ATTR_DRLC_DEVICE_CLASS_VALUE | DEVICE CLASS VALUE | unknown |  |

## 0x0B01 Meter Identification cluster identifier (METER_IDENTIFICATION)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_METER_IDENTIFICATION_COMPANY_NAME_ID | COMPANY NAME ID | unknown |  |
| 0x0001 | ESP_ZB_ZCL_ATTR_METER_IDENTIFICATION_METER_TYPE_ID_ID | METER TYPE ID ID | unknown |  |
| 0x0004 | ESP_ZB_ZCL_ATTR_METER_IDENTIFICATION_DATA_QUALITY_ID_ID | DATA QUALITY ID ID | unknown |  |
| 0x0005 | ESP_ZB_ZCL_ATTR_METER_IDENTIFICATION_CUSTOMER_NAME_ID | CUSTOMER NAME ID | unknown |  |
| 0x0006 | ESP_ZB_ZCL_ATTR_METER_IDENTIFICATION_MODEL_ID | MODEL ID | unknown |  |
| 0x0007 | ESP_ZB_ZCL_ATTR_METER_IDENTIFICATION_PART_NUMBER_ID | PART NUMBER ID | unknown |  |
| 0x0008 | ESP_ZB_ZCL_ATTR_METER_IDENTIFICATION_PRODUCT_REVISION_ID | PRODUCT REVISION ID | unknown |  |
| 0x000A | ESP_ZB_ZCL_ATTR_METER_IDENTIFICATION_SOFTWARE_REVISION_ID | SOFTWARE REVISION ID | unknown |  |
| 0x000B | ESP_ZB_ZCL_ATTR_METER_IDENTIFICATION_UTILITY_NAME_ID | UTILITY NAME ID | unknown |  |
| 0x000C | ESP_ZB_ZCL_ATTR_METER_IDENTIFICATION_POD_ID | POD ID | unknown |  |
| 0x000D | ESP_ZB_ZCL_ATTR_METER_IDENTIFICATION_AVAILABLE_POWER_ID | AVAILABLE POWER ID | unknown |  |
| 0x000E | ESP_ZB_ZCL_ATTR_METER_IDENTIFICATION_POWER_THRESHOLD_ID | POWER THRESHOLD ID | unknown |  |

## 0x0B04 Electrical measurement (ELECTRICAL_MEASUREMENT)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_MEASUREMENT_TYPE_ID | MEASUREMENT TYPE ID | unknown | This attribute indicates a device s measurement capabilities. |
| 0x0100 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_VOLTAGE_ID | DC VOLTAGE ID | unknown | The DCVoltage attribute represents the most recent DC voltage reading in Volts (V). |
| 0x0101 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_VOLTAGE_MIN_ID | DC VOLTAGE MIN ID | unknown | The DCVoltageMin attribute represents the lowest DC voltage value measured in Volts (V). |
| 0x0102 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_VOLTAGE_MAX_ID | DC VOLTAGE MAX ID | unknown | The DCVoltageMax attribute represents the highest DC voltage value measured in Volts (V). |
| 0x0103 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_CURRENT_ID | DC CURRENT ID | unknown | The DCCurrent attribute represents the most recent DC current reading in Amps (A). |
| 0x0104 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_CURRENT_MIN_ID | DC CURRENT MIN ID | unknown | The DCCurrentMin attribute represents the lowest DC current value measured in Amps (A). |
| 0x0105 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_CURRENT_MAX_ID | DC CURRENT MAX ID | unknown | The DCCurrentMax attribute represents the highest DC current value measured in Amps (A). |
| 0x0106 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DCPOWER_ID | DCPOWER ID | unknown | The @e DCPower attribute represents the most recent DC power reading in @e Watts (W) |
| 0x0107 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_POWER_MIN_ID | DC POWER MIN ID | unknown | The DCPowerMin attribute represents the lowest DC power value measured in Watts (W). |
| 0x0108 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_POWER_MAX_ID | DC POWER MAX ID | unknown | The DCPowerMax attribute represents the highest DC power value measured in Watts (W). |
| 0x0200 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_VOLTAGE_MULTIPLIER_ID | DC VOLTAGE MULTIPLIER ID | unknown | The DCVoltageMultiplier provides a value to be multiplied against the DCVoltage, DCVoltageMin, and DCVoltageMax attributes. |
| 0x0201 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_VOLTAGE_DIVISOR_ID | DC VOLTAGE DIVISOR ID | unknown | The DCVoltageDivisor provides a value to be divided against the DCVoltage, DCVoltageMin, and DCVoltageMax attributes. |
| 0x0202 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_CURRENT_MULTIPLIER_ID | DC CURRENT MULTIPLIER ID | unknown | The DCCurrentMultiplier provides a value to be multiplied against the DCCurrent, DCCurrentMin, and DCCurrentMax attributes. |
| 0x0203 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_CURRENT_DIVISOR_ID | DC CURRENT DIVISOR ID | unknown | The DCCurrentDivisor provides a value to be divided against the DCCurrent, DCCurrentMin, and DCCurrentMax attributes. |
| 0x0204 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_POWER_MULTIPLIER_ID | DC POWER MULTIPLIER ID | unknown | The DCPowerMultiplier provides a value to be multiplied against the DCPower, DCPowerMin, and DCPowerMax attributes. |
| 0x0205 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_POWER_DIVISOR_ID | DC POWER DIVISOR ID | unknown | The DCPowerDivisor provides a value to be divided against the DCPower, DCPowerMin, and DCPowerMax attributes. |
| 0x0300 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_FREQUENCY_ID | AC FREQUENCY ID | unknown | The ACFrequency attribute represents the most recent AC Frequency reading in Hertz (Hz). |
| 0x0301 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_FREQUENCY_MIN_ID | AC FREQUENCY MIN ID | unknown | The ACFrequencyMin attribute represents the lowest AC Frequency value measured in Hertz (Hz). |
| 0x0302 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_FREQUENCY_MAX_ID | AC FREQUENCY MAX ID | unknown | The ACFrequencyMax attribute represents the highest AC Frequency value measured in Hertz (Hz). |
| 0x0303 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_NEUTRAL_CURRENT_ID | NEUTRAL CURRENT ID | unknown | The NeutralCurrent attribute represents the AC neutral (Line-Out) current value at the moment in time the attribute is read, in Amps (A). |
| 0x0304 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_TOTAL_ACTIVE_POWER_ID | TOTAL ACTIVE POWER ID | unknown | Active power represents the current demand of active power delivered or received at the premises, in @e kW |
| 0x0305 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_TOTAL_REACTIVE_POWER_ID | TOTAL REACTIVE POWER ID | unknown | Reactive power represents the current demand of reactive power delivered or received at the premises, in kVAr. |
| 0x0306 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_TOTAL_APPARENT_POWER_ID | TOTAL APPARENT POWER ID | unknown | Represents the current demand of apparent power, in @e kVA |
| 0x0307 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_MEASURED1ST_HARMONIC_CURRENT_ID | MEASURED1ST HARMONIC CURRENT ID | unknown | Attribute represent the most recent 1st harmonic current reading in an AC frequency. |
| 0x0308 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_MEASURED3RD_HARMONIC_CURRENT_ID | MEASURED3RD HARMONIC CURRENT ID | unknown | Attribute represent the most recent 3rd harmonic current reading in an AC frequency. |
| 0x0309 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_MEASURED5TH_HARMONIC_CURRENT_ID | MEASURED5TH HARMONIC CURRENT ID | unknown | Attribute represent the most recent 5th harmonic current reading in an AC frequency. |
| 0x030A | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_MEASURED7TH_HARMONIC_CURRENT_ID | MEASURED7TH HARMONIC CURRENT ID | unknown | Attribute represent the most recent 7th harmonic current reading in an AC frequency. |
| 0x030B | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_MEASURED9TH_HARMONIC_CURRENT_ID | MEASURED9TH HARMONIC CURRENT ID | unknown | Attribute represent the most recent 9th harmonic current reading in an AC frequency. |
| 0x030C | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_MEASURED11TH_HARMONIC_CURRENT_ID | MEASURED11TH HARMONIC CURRENT ID | unknown | Attribute represent the most recent 11th harmonic current reading in an AC frequency. |
| 0x030D | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_MEASURED_PHASE1ST_HARMONIC_CURRENT_ID | MEASURED PHASE1ST HARMONIC CURRENT ID | unknown | Attribute represent the most recent phase of the 1st harmonic current reading in an AC frequency. |
| 0x030E | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_MEASURED_PHASE3RD_HARMONIC_CURRENT_ID | MEASURED PHASE3RD HARMONIC CURRENT ID | unknown | Attribute represent the most recent phase of the 3rd harmonic current reading in an AC frequency. |
| 0x030F | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_MEASURED_PHASE5TH_HARMONIC_CURRENT_ID | MEASURED PHASE5TH HARMONIC CURRENT ID | unknown | Attribute represent the most recent phase of the 5th harmonic current reading in an AC frequency. |
| 0x0310 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_MEASURED_PHASE7TH_HARMONIC_CURRENT_ID | MEASURED PHASE7TH HARMONIC CURRENT ID | unknown | Attribute represent the most recent phase of the 7th harmonic current reading in an AC frequency. |
| 0x0311 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_MEASURED_PHASE9TH_HARMONIC_CURRENT_ID | MEASURED PHASE9TH HARMONIC CURRENT ID | unknown | Attribute represent the most recent phase of the 9th harmonic current reading in an AC frequency. |
| 0x0312 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_MEASURED_PHASE11TH_HARMONIC_CURRENT_ID | MEASURED PHASE11TH HARMONIC CURRENT ID | unknown | Attribute represent the most recent phase of the 11th harmonic current reading in an AC frequency. |
| 0x0400 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_FREQUENCY_MULTIPLIER_ID | AC FREQUENCY MULTIPLIER ID | unknown | Provides a value to be multiplied against the ACFrequency attribute. |
| 0x0401 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_FREQUENCY_DIVISOR_ID | AC FREQUENCY DIVISOR ID | unknown | Provides a value to be divided against the ACFrequency attribute. |
| 0x0402 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_POWER_MULTIPLIER_ID | POWER MULTIPLIER ID | unknown | Provides a value to be multiplied against a raw or uncompensated sensor count of power being measured by the metering device. |
| 0x0403 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_POWER_DIVISOR_ID | POWER DIVISOR ID | unknown | Provides a value to divide against the results of applying the @e Multiplier attribute against a raw or uncompensated sensor count of power being measured by the metering device. |
| 0x0404 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_HARMONIC_CURRENT_MULTIPLIER_ID | HARMONIC CURRENT MULTIPLIER ID | unknown | Represents the unit value for the MeasuredNthHarmonicCurrent attribute in the format MeasuredNthHarmonicCurrent * 10 ^ HarmonicCurrentMultiplier amperes. |
| 0x0405 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_PHASE_HARMONIC_CURRENT_MULTIPLIER_ID | PHASE HARMONIC CURRENT MULTIPLIER ID | unknown | Represents the unit value for the MeasuredPhaseNthHarmonicCurrent attribute in the format MeasuredPhaseNthHarmonicCurrent * 10 ^ PhaseHarmonicCurrentMultiplier degrees. |
| 0x0501 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_LINE_CURRENT_ID | LINE CURRENT ID | unknown | Represents the single phase or Phase A, AC line current (Square root of active and reactive current) value at the moment in time the attribute is read, in @e Amps (A). |
| 0x0502 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_CURRENT_ID | ACTIVE CURRENT ID | unknown | Represents the single phase or Phase A, AC active/resistive current value at the moment in time the attribute is read, in Amps (A). |
| 0x0503 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_REACTIVE_CURRENT_ID | REACTIVE CURRENT ID | unknown | Represents the single phase or Phase A, AC reactive current value at the moment in time the attribute is read, in Amps (A). |
| 0x0505 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMSVOLTAGE_ID | RMSVOLTAGE ID | unknown | Represents the most recent RMS voltage reading in @e Volts (V). |
| 0x0506 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_MIN_ID | RMS VOLTAGE MIN ID | unknown | Represents the lowest RMS voltage value measured in Volts (V). |
| 0x0507 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_MAX_ID | RMS VOLTAGE MAX ID | unknown | Represents the highest RMS voltage value measured in Volts (V). |
| 0x0508 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMSCURRENT_ID | RMSCURRENT ID | unknown | Represents the most recent RMS current reading in @e Amps (A). |
| 0x0509 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_CURRENT_MIN_ID | RMS CURRENT MIN ID | unknown | Represents the lowest RMS current value measured in Amps (A). |
| 0x050A | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_CURRENT_MAX_ID | RMS CURRENT MAX ID | unknown | Represents the highest RMS current value measured in Amps (A). |
| 0x050B | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID | ACTIVE POWER ID | unknown | Represents the single phase or Phase A, current demand of active power delivered or received at the premises, in @e Watts (W). |
| 0x050C | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_MIN_ID | ACTIVE POWER MIN ID | unknown | Represents the lowest AC power value measured in Watts (W). |
| 0x050D | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_MAX_ID | ACTIVE POWER MAX ID | unknown | Represents the highest AC power value measured in Watts (W). |
| 0x050E | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_REACTIVE_POWER_ID | REACTIVE POWER ID | unknown | Represents the single phase or Phase A, current demand of reactive power delivered or received at the premises, in VAr. |
| 0x050F | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_APPARENT_POWER_ID | APPARENT POWER ID | unknown | Represents the single phase or Phase A, current demand of apparent (Square root of active and reactive power) power, in @e VA. |
| 0x0510 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_POWER_FACTOR_ID | POWER FACTOR ID | unknown | Contains the single phase or PhaseA, Power Factor ratio in 1/100th. |
| 0x0511 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AVERAGE_RMSVOLTAGE_MEASUREMENT_PERIOD_ID | AVERAGE RMSVOLTAGE MEASUREMENT PERIOD ID | unknown | The Period in seconds that the RMS voltage is averaged over. |
| 0x0512 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AVERAGE_RMS_OVER_VOLTAGE_COUNTER_ID | AVERAGE RMS OVER VOLTAGE COUNTER ID | unknown | The number of times the average RMS voltage, has been above the AverageRMS OverVoltage threshold since last reset. |
| 0x0513 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AVERAGE_RMS_UNDER_VOLTAGE_COUNTER_ID | AVERAGE RMS UNDER VOLTAGE COUNTER ID | unknown | The number of times the average RMS voltage, has been below the AverageRMS underVoltage threshold since last reset. |
| 0x0514 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_EXTREME_OVER_VOLTAGE_PERIOD_ID | RMS EXTREME OVER VOLTAGE PERIOD ID | unknown | The duration in seconds used to measure an extreme over voltage condition. |
| 0x0515 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_EXTREME_UNDER_VOLTAGE_PERIOD_ID | RMS EXTREME UNDER VOLTAGE PERIOD ID | unknown | The duration in seconds used to measure an extreme under voltage condition. |
| 0x0516 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_SAG_PERIOD_ID | RMS VOLTAGE SAG PERIOD ID | unknown | The duration in seconds used to measure a voltage sag condition. |
| 0x0517 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_SWELL_PERIOD_ID | RMS VOLTAGE SWELL PERIOD ID | unknown | The duration in seconds used to measure a voltage swell condition. |
| 0x0600 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACVOLTAGE_MULTIPLIER_ID | ACVOLTAGE MULTIPLIER ID | unknown | Provides a value to be multiplied against the @e InstantaneousVoltage and RMSVoltage attributes. |
| 0x0601 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACVOLTAGE_DIVISOR_ID | ACVOLTAGE DIVISOR ID | unknown | Provides a value to be divided against the @e InstantaneousVoltage |
| 0x0602 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACCURRENT_MULTIPLIER_ID | ACCURRENT MULTIPLIER ID | unknown | Provides a value to be multiplied against the @e InstantaneousCurrent and @e RMSCurrent attributes |
| 0x0603 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACCURRENT_DIVISOR_ID | ACCURRENT DIVISOR ID | unknown | Provides a value to be divided against the @e ACCurrent, @e InstantaneousCurrent and @e RMSCurrent attributes. |
| 0x0604 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACPOWER_MULTIPLIER_ID | ACPOWER MULTIPLIER ID | unknown | Provides a value to be multiplied against the @e InstantaneousPower and @e ActivePower attributes |
| 0x0605 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACPOWER_DIVISOR_ID | ACPOWER DIVISOR ID | unknown | Provides a value to be divided against the @e InstantaneousPower and @e ActivePower attributes. |
| 0x0700 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_OVERLOAD_ALARMS_MASK_ID | DC OVERLOAD ALARMS MASK ID | unknown | Specifies which configurable alarms may be generated. |
| 0x0701 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_VOLTAGE_OVERLOAD_ID | DC VOLTAGE OVERLOAD ID | unknown | Specifies the alarm threshold, set by the manufacturer, for the maximum output voltage supported by device. |
| 0x0702 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_DC_CURRENT_OVERLOAD_ID | DC CURRENT OVERLOAD ID | unknown | Specifies the alarm threshold, set by the manufacturer, for the maximum output current supported by device. |
| 0x0800 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_ALARMS_MASK_ID | AC ALARMS MASK ID | unknown | Specifies which configurable alarms may be generated. |
| 0x0801 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_VOLTAGE_OVERLOAD_ID | AC VOLTAGE OVERLOAD ID | unknown | Specifies the alarm threshold, set by the manufacturer, for the maximum output voltage supported by device. |
| 0x0802 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_CURRENT_OVERLOAD_ID | AC CURRENT OVERLOAD ID | unknown | Specifies the alarm threshold, set by the manufacturer, for the maximum output current supported by device. |
| 0x0803 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_ACTIVE_POWER_OVERLOAD_ID | AC ACTIVE POWER OVERLOAD ID | unknown | Specifies the alarm threshold, set by the manufacturer, for the maximum output active power supported by device. |
| 0x0804 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_REACTIVE_POWER_OVERLOAD_ID | AC REACTIVE POWER OVERLOAD ID | unknown | Specifies the alarm threshold, set by the manufacturer, for the maximum output reactive power supported by device. |
| 0x0805 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AVERAGE_RMS_OVER_VOLTAGE_ID | AVERAGE RMS OVER VOLTAGE ID | unknown | The average RMS voltage above which an over voltage condition is reported. |
| 0x0806 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AVERAGE_RMS_UNDER_VOLTAGE_ID | AVERAGE RMS UNDER VOLTAGE ID | unknown | The average RMS voltage below which an under voltage condition is reported. |
| 0x0807 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_EXTREME_OVER_VOLTAGE_ID | RMS EXTREME OVER VOLTAGE ID | unknown | The RMS voltage above which an extreme under voltage condition is reported. |
| 0x0808 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_EXTREME_UNDER_VOLTAGE_ID | RMS EXTREME UNDER VOLTAGE ID | unknown | The RMS voltage below which an extreme under voltage condition is reported. |
| 0x0809 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_SAG_ID | RMS VOLTAGE SAG ID | unknown | The RMS voltage below which a sag condition is reported. |
| 0x080A | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_SWELL_ID | RMS VOLTAGE SWELL ID | unknown | The RMS voltage above which a swell condition is reported. |
| 0x0901 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_LINE_CURRENT_PH_B_ID | LINE CURRENT PH B ID | unknown | Represents the Phase B, AC line current (Square root sum of active and reactive currents) value at the moment in time the attribute is read, in Amps (A). |
| 0x0902 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_CURRENT_PH_B_ID | ACTIVE CURRENT PH B ID | unknown | Represents the Phase B, AC active/resistive current value at the moment in time |
| 0x0903 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_REACTIVE_CURRENT_PH_B_ID | REACTIVE CURRENT PH B ID | unknown | Represents the Phase B, AC reactive current value at the moment in time the |
| 0x0905 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMSVOLTAGE_PHB_ID | RMSVOLTAGE PHB ID | unknown | Represents the most recent RMS voltage reading in @e Volts (V). |
| 0x0906 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_MIN_PH_B_ID | RMS VOLTAGE MIN PH B ID | unknown | Represents the lowest RMS voltage value measured in Volts (V). |
| 0x0907 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_MAX_PH_B_ID | RMS VOLTAGE MAX PH B ID | unknown | Represents the highest RMS voltage value measured in Volts (V). |
| 0x0908 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMSCURRENT_PHB_ID | RMSCURRENT PHB ID | unknown | Represents the most recent RMS current reading in @e Amps (A). |
| 0x0909 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_CURRENT_MIN_PH_B_ID | RMS CURRENT MIN PH B ID | unknown | Represents the lowest RMS current value measured in Amps (A). |
| 0x090A | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_CURRENT_MAX_PH_B_ID | RMS CURRENT MAX PH B ID | unknown | Represents the highest RMS current value measured in Amps (A). |
| 0x090B | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_PHB_ID | ACTIVE POWER PHB ID | unknown | Represents the Phase B, current demand of active power delivered or received at the premises, in @e Watts (W). |
| 0x090C | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_MIN_PH_B_ID | ACTIVE POWER MIN PH B ID | unknown | Represents the lowest AC power value measured in Watts (W). |
| 0x090D | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_MAX_PH_B_ID | ACTIVE POWER MAX PH B ID | unknown | Represents the highest AC power value measured in Watts (W). |
| 0x090E | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_REACTIVE_POWER_PH_B_ID | REACTIVE POWER PH B ID | unknown | Represents the Phase B, current demand of reactive power delivered or received at the premises, in VAr. |
| 0x090F | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_APPARENT_POWER_PHB_ID | APPARENT POWER PHB ID | unknown | Represents the Phase B, current demand of apparent (Square root of active and reactive power) power, in @e VA. |
| 0x0910 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_POWER_FACTOR_PH_B_ID | POWER FACTOR PH B ID | unknown | Contains the PhaseB, Power Factor ratio in 1/100th. |
| 0x0911 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AVERAGE_RMSVOLTAGE_MEASUREMENT_PERIOD_PHB_ID | AVERAGE RMSVOLTAGE MEASUREMENT PERIOD PHB ID | unknown | The number of times the average RMS voltage, has been above the @e AverageRMS @e OverVoltage threshold since last reset. |
| 0x0912 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AVERAGE_RMS_OVER_VOLTAGE_COUNTER_PH_B_ID | AVERAGE RMS OVER VOLTAGE COUNTER PH B ID | unknown | The number of times the average RMS voltage, has been above the AverageRMS OverVoltage threshold since last reset. |
| 0x0913 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AVERAGE_RMS_UNDER_VOLTAGE_COUNTER_PH_B_ID | AVERAGE RMS UNDER VOLTAGE COUNTER PH B ID | unknown | The number of times the average RMS voltage, has been below the AverageRMS underVoltage threshold since last reset. |
| 0x0914 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_EXTREME_OVER_VOLTAGE_PERIOD_PH_B_ID | RMS EXTREME OVER VOLTAGE PERIOD PH B ID | unknown | The duration in seconds used to measure an extreme over voltage condition. |
| 0x0915 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_EXTREME_UNDER_VOLTAGE_PERIOD_PH_B_ID | RMS EXTREME UNDER VOLTAGE PERIOD PH B ID | unknown | The duration in seconds used to measure an extreme under voltage condition. |
| 0x0916 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_SAG_PERIOD_PH_B_ID | RMS VOLTAGE SAG PERIOD PH B ID | unknown | The duration in seconds used to measure a voltage sag condition. |
| 0x0917 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_SWELL_PERIOD_PH_B_ID | RMS VOLTAGE SWELL PERIOD PH B ID | unknown | The duration in seconds used to measure a voltage swell condition. |
| 0x0A01 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_LINE_CURRENT_PH_C_ID | LINE CURRENT PH C ID | unknown | Represents the Phase C, AC line current (Square root of active and reactive current) value at the moment in time the attribute is read, in Amps (A). |
| 0x0A02 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_CURRENT_PH_C_ID | ACTIVE CURRENT PH C ID | unknown | Represents the Phase C, AC active/resistive current value at the moment in time the attribute is read, in Amps (A). |
| 0x0A03 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_REACTIVE_CURRENT_PH_C_ID | REACTIVE CURRENT PH C ID | unknown | Represents the Phase C, AC reactive current value at the moment in time the attribute is read, in Amps (A). |
| 0x0A05 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMSVOLTAGE_PHC_ID | RMSVOLTAGE PHC ID | unknown | Represents the most recent RMS voltage reading in @e Volts (V). |
| 0x0A06 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_MIN_PH_C_ID | RMS VOLTAGE MIN PH C ID | unknown | Represents the lowest RMS voltage value measured in Volts (V). |
| 0x0A07 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_MAX_PH_C_ID | RMS VOLTAGE MAX PH C ID | unknown | Represents the highest RMS voltage value measured in Volts (V). |
| 0x0A08 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMSCURRENT_PHC_ID | RMSCURRENT PHC ID | unknown | Represents the most recent RMS current reading in @e Amps (A). |
| 0x0A09 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_CURRENT_MIN_PH_C_ID | RMS CURRENT MIN PH C ID | unknown | Represents the lowest RMS current value measured in Amps (A). |
| 0x0A0A | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_CURRENT_MAX_PH_C_ID | RMS CURRENT MAX PH C ID | unknown | Represents the highest RMS current value measured in Amps (A). |
| 0x0A0B | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_PHC_ID | ACTIVE POWER PHC ID | unknown | Represents the Phase C, current demand of active power delivered or received at the premises, in @e Watts (W). |
| 0x0A0C | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_MIN_PH_C_ID | ACTIVE POWER MIN PH C ID | unknown | Represents the lowest AC power value measured in Watts (W). |
| 0x0A0D | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_MAX_PH_C_ID | ACTIVE POWER MAX PH C ID | unknown | Represents the highest AC power value measured in Watts (W). |
| 0x0A0E | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_REACTIVE_POWER_PH_C_ID | REACTIVE POWER PH C ID | unknown | Represents the Phase C, current demand of reactive power delivered or received at the premises, in VAr. |
| 0x0A0F | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_APPARENT_POWER_PHC_ID | APPARENT POWER PHC ID | unknown | Represents the Phase C, current demand of apparent (Square root of active and reactive power) power, in @e VA. |
| 0x0A10 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_POWER_FACTOR_PH_C_ID | POWER FACTOR PH C ID | unknown | Contains the Phase C, Power Factor ratio in 1/100th. |
| 0x0A11 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AVERAGE_RMSVOLTAGE_MEASUREMENT_PERIOD_PHC_ID | AVERAGE RMSVOLTAGE MEASUREMENT PERIOD PHC ID | unknown | The Period in seconds that the RMS voltage is averaged over |
| 0x0A12 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AVERAGE_RMS_OVER_VOLTAGE_COUNTER_PH_C_ID | AVERAGE RMS OVER VOLTAGE COUNTER PH C ID | unknown | The number of times the average RMS voltage, has been above the AverageRMS OverVoltage threshold since last reset. |
| 0x0A13 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AVERAGE_RMS_UNDER_VOLTAGE_COUNTER_PH_C_ID | AVERAGE RMS UNDER VOLTAGE COUNTER PH C ID | unknown | The number of times the average RMS voltage, has been below the AverageRMS underVoltage threshold since last reset. |
| 0x0A14 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_EXTREME_OVER_VOLTAGE_PERIOD_PH_C_ID | RMS EXTREME OVER VOLTAGE PERIOD PH C ID | unknown | The duration in seconds used to measure an extreme over voltage condition. |
| 0x0A15 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_EXTREME_UNDER_VOLTAGE_PERIOD_PH_C_ID | RMS EXTREME UNDER VOLTAGE PERIOD PH C ID | unknown | The duration in seconds used to measure an extreme under voltage condition. |
| 0x0A16 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_SAG_PERIOD_PH_C_ID | RMS VOLTAGE SAG PERIOD PH C ID | unknown | The duration in seconds used to measure a voltage sag condition. |
| 0x0A17 | ESP_ZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_SWELL_PERIOD_PH_C_ID | RMS VOLTAGE SWELL PERIOD PH C ID | unknown | The duration in seconds used to measure a voltage swell condition. |

## 0x0B05 Home Automation Diagnostics (DIAGNOSTICS)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_NUMBER_OF_RESETS_ID | NUMBER OF RESETS ID | unknown | Number Of Resets |
| 0x0001 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_PERSISTENT_MEMORY_WRITES_ID | PERSISTENT MEMORY WRITES ID | unknown | Persistent Memory Writes |
| 0x0100 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_MAC_RX_BCAST_ID | MAC RX BCAST ID | unknown | MAC Rx Broadcast |
| 0x0101 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_MAC_TX_BCAST_ID | MAC TX BCAST ID | unknown | MAC Tx Broadcast |
| 0x0102 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_MAC_RX_UCAST_ID | MAC RX UCAST ID | unknown | MAC Rx Unicast |
| 0x0103 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_MAC_TX_UCAST_ID | MAC TX UCAST ID | unknown | MAC Tx Unicast |
| 0x0104 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_MAC_TX_UCAST_RETRY_ID | MAC TX UCAST RETRY ID | unknown | MAC Tx Unicast Retry |
| 0x0105 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_MAC_TX_UCAST_FAIL_ID | MAC TX UCAST FAIL ID | unknown | MAC Tx Unicast Fail |
| 0x0106 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_APS_RX_BCAST_ID | APS RX BCAST ID | unknown | APS Rx Broadcast |
| 0x0107 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_APS_TX_BCAST_ID | APS TX BCAST ID | unknown | APS Tx Broadcast |
| 0x0108 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_APS_RX_UCAST_ID | APS RX UCAST ID | unknown | APS Rx Unicast |
| 0x0109 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_APS_TX_UCAST_SUCCESS_ID | APS TX UCAST SUCCESS ID | unknown | APS Tx Unicast Success |
| 0x010A | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_APS_TX_UCAST_RETRY_ID | APS TX UCAST RETRY ID | unknown | APS Tx Unicast Retry |
| 0x010B | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_APS_TX_UCAST_FAIL_ID | APS TX UCAST FAIL ID | unknown | APS Tx Unicast Fail |
| 0x010C | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_ROUTE_DISC_INITIATED_ID | ROUTE DISC INITIATED ID | unknown | Route Disc Initiated |
| 0x010D | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_NEIGHBOR_ADDED_ID | NEIGHBOR ADDED ID | unknown | Neighbor Added |
| 0x010E | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_NEIGHBOR_REMOVED_ID | NEIGHBOR REMOVED ID | unknown | Neighbor Removed |
| 0x010F | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_NEIGHBOR_STALE_ID | NEIGHBOR STALE ID | unknown | Neighbor Stale |
| 0x0110 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_JOIN_INDICATION_ID | JOIN INDICATION ID | unknown | Join Indication |
| 0x0111 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_CHILD_MOVED_ID | CHILD MOVED ID | unknown | Child Moved |
| 0x0112 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_NWKFC_FAILURE_ID | NWKFC FAILURE ID | unknown | NWKFC Failure |
| 0x0113 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_APSFC_FAILURE_ID | APSFC FAILURE ID | unknown | APSFC Failure |
| 0x0114 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_APS_UNAUTHORIZED_KEY_ID | APS UNAUTHORIZED KEY ID | unknown | APS Unauthorized Key |
| 0x0115 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_NWK_DECRYPT_FAILURES_ID | NWK DECRYPT FAILURES ID | unknown | NWK Decrypt Failures |
| 0x0116 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_APS_DECRYPT_FAILURES_ID | APS DECRYPT FAILURES ID | unknown | APS Decrypt Failures |
| 0x0117 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_PACKET_BUFFER_ALLOCATE_FAILURES_ID | PACKET BUFFER ALLOCATE FAILURES ID | unknown | Packet Buffer Allocate Failures |
| 0x0118 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_RELAYED_UCAST_ID | RELAYED UCAST ID | unknown | Relayed Unicast |
| 0x0119 | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_PHYTOMACQUEUELIMITREACHED_ID | PHYTOMACQUEUELIMITREACHED ID | unknown | Phytomacqueuelimitreached |
| 0x011A | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_PACKET_VALIDATEDROPCOUNT_ID | PACKET VALIDATEDROPCOUNT ID | unknown | Packet Validatedropcount |
| 0x011B | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_AVERAGE_MAC_RETRY_PER_APS_ID | AVERAGE MAC RETRY PER APS ID | unknown | Average MAC Retry Per APS |
| 0x011C | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_LAST_LQI_ID | LAST LQI ID | unknown | Last LQI |
| 0x011D | ESP_ZB_ZCL_ATTR_DIAGNOSTICS_LAST_RSSI_ID | LAST RSSI ID | unknown | Last RSSI |

## n/a CUSTOM CIE (CUSTOM_CIE)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0xE000 | ESP_ZB_ZCL_ATTR_CUSTOM_CIE_ADDR_IS_SET | ADDR IS SET | unknown | Custom CIE address for checking establishment and authorization internally |
| 0xE001 | ESP_ZB_ZCL_ATTR_CUSTOM_CIE_EP | EP | unknown | Custom CIE endpoint for checking establishment and authorization internally |
| 0xE002 | ESP_ZB_ZCL_ATTR_CUSTOM_CIE_SHORT_ADDR | SHORT ADDR | unknown | Custom CIE short address for checking establishment and authorization internally |

## n/a GP LINK (GP_LINK)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0022 | ESP_ZB_ZCL_ATTR_GP_LINK_KEY_ID | KEY ID | unknown | The security key to be used to encrypt the key exchanged with the GPD |

## n/a GP SHARED (GP_SHARED)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0020 | ESP_ZB_ZCL_ATTR_GP_SHARED_SECURITY_KEY_TYPE_ID | SECURITY KEY TYPE ID | unknown | The security key type to be used for the communication with all paired 0b11 GPD in this network |
| 0x0021 | ESP_ZB_ZCL_ATTR_GP_SHARED_SECURITY_KEY_ID | SECURITY KEY ID | unknown | The security key to be used for the communication with all paired GPD in this network |

## n/a GPP ACTIVE (GPP_ACTIVE)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0017 | ESP_ZB_ZCL_ATTR_GPP_ACTIVE_FUNCTIONALITY_ID | FUNCTIONALITY ID | unknown | The optional GP functionality supported by this GPP that is active |

## n/a GPP BLOCKED (GPP_BLOCKED)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0015 | ESP_ZB_ZCL_ATTR_GPP_BLOCKED_GPDID_ID | GPDID ID | unknown | A list holding information about blocked GPD IDs |

## n/a GPP FUNCTIONALITY (GPP_FUNCTIONALITY)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0016 | ESP_ZB_ZCL_ATTR_GPP_FUNCTIONALITY_ID | ID | unknown | The optional GP functionality supported by this GPP |

## n/a GPP MAX (GPP_MAX)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0010 | ESP_ZB_ZCL_ATTR_GPP_MAX_PROXY_TABLE_ENTRIES_ID | PROXY TABLE ENTRIES ID | unknown | Maximum number of Proxy Table entries supported by this device |
| 0x0014 | ESP_ZB_ZCL_ATTR_GPP_MAX_SEARCH_COUNTER_ID | SEARCH COUNTER ID | unknown | The frequency of sink re-discovery for inactive Proxy Table entries |

## n/a GPP NOTIFICATION (GPP_NOTIFICATION)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0012 | ESP_ZB_ZCL_ATTR_GPP_NOTIFICATION_RETRY_NUMBER_ID | RETRY NUMBER ID | unknown | Number of unicast GP Notification retries on lack of GP Notification Response |
| 0x0013 | ESP_ZB_ZCL_ATTR_GPP_NOTIFICATION_RETRY_TIMER_ID | RETRY TIMER ID | unknown | Time in ms between unicast GP Notification retries on lack of GP Notification Response |

## n/a GPP PROXY (GPP_PROXY)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0011 | ESP_ZB_ZCL_ATTR_GPP_PROXY_TABLE_ID | TABLE ID | unknown | Proxy Table, holding information about pairings between a particular GPD ID and GPSs in the network |

## n/a GPS ACTIVE (GPS_ACTIVE)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0007 | ESP_ZB_ZCL_ATTR_GPS_ACTIVE_FUNCTIONALITY_ID | FUNCTIONALITY ID | unknown | The optional GP functionality supported by this GPS that is active |

## n/a GPS COMMISSIONING (GPS_COMMISSIONING)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0003 | ESP_ZB_ZCL_ATTR_GPS_COMMISSIONING_EXIT_MODE_ID | EXIT MODE ID | unknown | Conditions for the GPS to exit the commissioning mode |
| 0x0004 | ESP_ZB_ZCL_ATTR_GPS_COMMISSIONING_WINDOW_ID | WINDOW ID | unknown | Default duration of the Commissioning window duration, in seconds, as re- quested by this GPS |

## n/a GPS COMMUNICATION (GPS_COMMUNICATION)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0002 | ESP_ZB_ZCL_ATTR_GPS_COMMUNICATION_MODE_ID | MODE ID | unknown | Default communication mode requested by this GPS |

## n/a GPS FUNCTIONALITY (GPS_FUNCTIONALITY)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0006 | ESP_ZB_ZCL_ATTR_GPS_FUNCTIONALITY_ID | ID | unknown | The optional GP functionality supported by this GPS |

## n/a GPS MAX (GPS_MAX)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0000 | ESP_ZB_ZCL_ATTR_GPS_MAX_SINK_TABLE_ENTRIES_ID | SINK TABLE ENTRIES ID | unknown | Maximum number of Sink Table entries supported by this device |

## n/a GPS SECURITY (GPS_SECURITY)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0005 | ESP_ZB_ZCL_ATTR_GPS_SECURITY_LEVEL_ID | LEVEL ID | unknown | The minimum required security level to be supported by the paired GPDs |

## n/a GPS SINK (GPS_SINK)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0001 | ESP_ZB_ZCL_ATTR_GPS_SINK_TABLE_ID | TABLE ID | unknown | Sink Table, holding information about local bindings between a particular GPD and target‘s local endpoints |

## n/a REL HUMIDITY (REL_HUMIDITY)

| Attr ID | SDK Symbol | Name | Type | Notes |
|---|---|---|---|---|
| 0x0003 | ESP_ZB_ZCL_ATTR_REL_HUMIDITY_TOLERANCE_ID | TOLERANCE ID | unknown | The Tolerance attribute SHALL indicate the magnitude of the possible error that is associated with MeasuredValue, using the same units and resolution. |


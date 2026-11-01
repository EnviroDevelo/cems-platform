"""Owner decisions that change what the generators make (INTEGRATION.md section 6).
Each entry carries the decision it comes from."""

# Q5 (Gary, 20 Oct): the range-alarm enables and limits are kept through power-off.
# In the original they are RAM, so every power-up left the range alarms off (finding K4).
PROMOTE_TO_SETTING = {
    "AnalogEnable": "Q5", "TemperatureEnable": "Q5",
    "AnalogAlarmEnabled": "Q5", "TemperatureAlarmEnabled": "Q5",
    "RangeMin": "Q5", "RangeMax": "Q5",
    "MinTemperatureRange": "Q5", "MaxTemperatureRange": "Q5",
    # Q6 (calibration retained): the RTD offset is the temperature channels' calibration
    "TemperatureShift": "Q6",
    # Channel labels and units shown with the readings. RAM in the original, so lost at every
    # power-up (50_HMI_Map U8); kept now that the readings are labelled on screen and web.
    "AnalogSensorUnits": "U8", "TemperatureSensorUnits": "U8",
    "AnalogName1": "U8", "AnalogName2": "U8", "TempName1": "U8", "TempName2": "U8",
}

# Q6 (Gary, 20 Oct): calibration values are retained: factory class, so "restore
# defaults" never clears them and they sit in the separate factory image.
FACTORY = {"FirstCalibPoint": "Q6", "SecondCalibPoint": "Q6", "TemperatureShift": "Q6"}

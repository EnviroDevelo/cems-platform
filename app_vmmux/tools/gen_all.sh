#!/bin/sh
# Regenerate every generated file of app_vmmux, in dependency order.
# Usage: sh app_vmmux/tools/gen_all.sh <path to Existing Design/Analysis>
set -e
A=$(dirname "$0"); D=$1
python3 "$A/gen_tags.py"   "$D/stage2/data_dictionary.csv"
python3 "$A/gen_params.py" "$D/stage2/data_dictionary.csv" "$D/stage5/hmi_elements.csv" "$A/../faithful/modbus_map.csv"
python3 "$A/gen_modbus.py" "$D/stage2/data_dictionary.csv" "$A/../faithful/modbus_map.csv"
python3 "$A/gen_params.py" "$D/stage2/data_dictionary.csv" "$D/stage5/hmi_elements.csv" "$A/../faithful/modbus_map.csv"
python3 "$A/gen_compat_desc.py" "$D/stage2/data_dictionary.csv"
python3 "$A/gen_modbus_doc.py"

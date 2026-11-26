# EV drive unit DU-1: service manual (simulated equipment)

## Overview and normal operating values
DU-1 is a traction drive: permanent-magnet motor, silicon-carbide inverter and a liquid cooling loop shared with the battery.
- Torque request (torque_request) and delivered motor torque (motor_torque): delivered torque normally within 5 Nm of the request.
- Inverter duty (inverter_duty): 40 to 70 percent at cruise.
- Phase current (phase_current): 150 to 220 A at cruise.
- Inverter temperature (inverter_temp): 55 to 75 degC. Warning at 95 degC, limp mode at 105 degC.
- Battery state of charge (battery_soc): power is limited below 15 percent.

## Fault W-200: inverter temperature high
Meaning: inverter temperature above 95 degC for 2 seconds. No torque limit yet.
Likely causes: coolant pump failure, low coolant level, blocked radiator, sustained high load such as towing or hill climbing.
Check: inverter_temp trend; a steady climb while phase_current is normal indicates lost cooling.

## Fault T-201: inverter over-temperature, limp mode
Meaning: inverter temperature above 105 degC for 2 seconds. Torque is limited to 40 percent of the request ("limp mode") to protect the power semiconductors. The vehicle remains drivable at reduced performance.
Most common cause: coolant pump failure (check pump fuse F21 and connector C14), then low coolant level.
Driver advice: drive gently to a safe stop or workshop; avoid hills and high speed.
Recovery: limp mode releases automatically after the temperature has been below about 97 degC for 5 seconds. The workshop must check the coolant pump before returning the vehicle to service.

## Fault B-310: low battery, power limited
Meaning: battery state of charge below 15 percent. Power is limited to 50 percent to protect the battery and extend range.
Action: charge as soon as possible. The limit clears once the state of charge rises above about 16 percent.

## Fault S-115: motor position sensor fault, safe torque-off
Meaning: delivered torque measured by the sensor differs from the inverter's torque estimate by more than 25 Nm for 0.8 seconds. Because the motor position may be wrong, the inverter applies safe torque-off: no drive torque; the vehicle coasts.
Likely causes: resolver (position sensor) wiring fault at connector C9, resolver misalignment after service, electromagnetic interference.
Driver advice: steer to a safe stop; braking and steering are unaffected.
Recovery: safe torque-off latches. A technician must inspect the resolver and clear the fault with service equipment. Foreman cannot clear faults or command torque.

## Control system
Torque control runs in a 1 kHz loop on the QNX real-time operating system at the highest priority. Foreman, the on-board assistant, runs at lower priority with read-only access to vehicle data and no ability to command the drive.

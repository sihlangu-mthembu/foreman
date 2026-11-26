# Packaging line PL-3: conveyor drive maintenance manual (simulated equipment)

## Overview and normal operating values
The PL-3 infeed conveyor is driven by a 7.5 kW induction motor on a variable-frequency drive. Normal operation at a line rate of 120 packs per minute:
- Motor speed (motor_speed): setpoint 1450 rpm, normally within plus or minus 10 rpm of setpoint.
- Drive output (drive_output): 60 to 75 percent.
- Motor current (motor_current): 12 to 15 A. The drive's current limit is 22 A.
- Motor winding temperature (motor_temp): 55 to 70 degC. Warning at 85 degC, derate at 95 degC.
- Conveyor load (conveyor_load): 35 to 50 percent with standard product.

## Alarm W05: conveyor overload
Meaning: conveyor load has been above 75 percent for more than 1 second.
Likely causes: heavier product batch, product build-up at the infeed, worn or dry conveyor bearings, belt tension too high.
What to check: compare conveyor_load and motor_current trends over the last 10 minutes; a slow rise suggests bearing or belt wear, a sudden step suggests a product change or build-up.
Action: the line keeps running. Reduce infeed rate or clear build-up at the next stop. If the load stays above 75 percent, schedule bearing inspection.

## Alarm E42: infeed jam (drive stopped at current limit)
Meaning: motor current exceeded 22 A for more than 0.3 seconds, so the drive tripped and the conveyor stopped.
Typical sequence: conveyor_load rises sharply, the drive raises its output to hold speed, motor current climbs past the limit, then E42 trips. W05 overload is often raised a moment before E42.
Safe recovery procedure:
1. Press the line STOP button and apply lockout/tagout on the drive isolator before reaching into the infeed.
2. Remove the jammed product and inspect the infeed guide rails and photo-eye for damage.
3. Rotate the drive pulley by hand to confirm the belt moves freely.
4. Remove lockout, then reset the alarm from the operator panel (RESET) and restart at reduced speed for one minute.
Note: Foreman cannot reset or restart the line. Resets are done only at the operator panel.

## Alarm W11: motor temperature high
Meaning: motor winding temperature above 85 degC for more than 2 seconds.
Likely causes: blocked cooling fan or fan failure, dirt on the motor fins, high ambient temperature, sustained overload.
What to check: motor_temp trend (a steady climb at constant current points to cooling loss), motor_current (high current points to overload).
Action: the line keeps running. Inspect the motor fan cowl and fins at the next opportunity.

## Alarm E17: motor over-temperature (line speed derated to 60 percent)
Meaning: motor winding temperature above 95 degC for more than 2 seconds. To protect the windings, the controller automatically reduces line speed to 60 percent of setpoint. Speed returns to normal by itself once the temperature has been below about 87 degC for 5 seconds.
Most common cause: cooling fan failure. A rising motor_temp while motor_current is normal or falling strongly indicates lost cooling rather than overload.
Recovery: keep the line running derated if production requires; check the fan (fuse F7, fan connector X12) at the next stop. Do not bypass the thermal protection.

## Alarm E08: speed sensor disagrees with drive model
Meaning: the measured motor speed differs from the drive's internal speed estimate by more than 100 rpm for 1.5 seconds. The motor is probably not running at the speed the encoder reports.
Likely causes: encoder coupling slipping, encoder cable damage or electrical noise, encoder failure.
Effect: because the control loop regulates the measured speed, the real conveyor speed drifts away from setpoint (usually slower). Product spacing will be wrong.
Action: stop the line at the next opportunity, check the encoder coupling set-screws and cable (connector X9). Replace the encoder if the fault persists.

## Restarting after any stop
Only a trained operator may restart the line, from the operator panel, after confirming the cause is removed. Foreman is read-only: it can explain what happened, but it has no ability to restart, reset or change the machine.

## Control system
The conveyor drive is controlled by a 1 kHz control loop running on the QNX real-time operating system. The loop has the highest scheduling priority on the controller; the Foreman assistant runs at a lower priority and cannot delay it. Control loop health can be checked with get_control_timing: deadline_misses should be 0.

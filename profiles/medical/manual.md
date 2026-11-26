# Infusion pump IP-2: technical reference (SIMULATION for software demonstration; not a medical device and not clinical guidance)

## Overview and normal operating values
IP-2 is a simulated volumetric infusion pump used to demonstrate isolation between a real-time controller and an AI assistant.
- Flow setpoint (flow_setpoint) and measured flow (measured_flow): measured flow normally within 3 mL/h of the setpoint.
- Pump motor drive (pump_motor): 40 to 60 percent at 125 mL/h.
- Downstream line pressure (line_pressure): 60 to 110 mmHg. Warning at 200 mmHg, occlusion alarm at 300 mmHg.
- Volume infused (volume_infused): cumulative volume delivered in this session.
- Motor current (motor_current): 120 to 160 mA.

## Alarm W3: downstream line pressure rising
Meaning: line pressure above 200 mmHg for 1 second. Infusion continues.
Likely causes: partially kinked or clamped tubing, closed downstream clamp, patient position.
Check: line_pressure trend. A steady climb at constant motor drive suggests a developing occlusion.

## Alarm A3: downstream occlusion, infusion stopped
Meaning: line pressure above 300 mmHg for 1 second. The pump stops delivering to prevent pressure build-up.
Procedure (simulation): a clinician checks the line from pump to patient for kinks and closed clamps, relieves the pressure, then restarts the infusion on the pump itself.
Foreman cannot restart the pump or change the rate; it can only explain the alarm.

## Alarm A5: air-in-line / flow sensor mismatch, infusion stopped
Meaning: measured flow differs from the pump's delivered-flow estimate by more than 30 mL/h for 0.5 seconds, which indicates air in the line at the flow sensor or a sensor fault. The pump stops.
Procedure (simulation): a clinician removes air from the line following local protocol, verifies the line is primed, then restarts on the pump.

## Control system
Flow control runs in a 1 kHz loop on the QNX real-time operating system at the highest priority. The assistant runs at a lower priority with read-only access and no command path to the pump.

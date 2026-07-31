# rkh-98-26.github.io

# SAT-RKH-1 — Real-Time Systems Final Capstone

## One sentence
This is a Real Time System simulating a satellite sensing the internal temperature of it's circuitry, sending that data 
over to be processed, and having that data sent over to Ground control on earth.

## Demo
- Video: 
- Live Wokwi: [Wokwi Link](https://wokwi.com/projects/471005401497528321)
- Website: [Website Link](https://rkh-98-26.github.io/)

## Architecture

![Concurrency Diagram](https://github.com/rkh-98-26/rkh-98-26.github.io/blob/main/Final%20Concurrency%20Diagram.png?raw=true)

The Temperature Sensor does a thorough check of the temperature of the satellite's internal circuitry. This data is 
formalized as a packet containing the timestamp, packet number, circuit voltage, and temperature. This packet is sent 
via a queue to the Temperature Monitor to simulate unwrapping the packet and checking whether the temperature is within 
acceptable limits and flags any packets that are not. This sets off the log coordinating, which flashes the data and sends 
it to ground control on earth with the antenna task, which can also be prompted by the button. This is all displayed on
the web monitor, which is protected by a mutex with the temperature monitor task to prevent reads during writes so that 
the packet data comes from the same packet.

## Tasks & timing (WCET evidence)

| Task               | Priority | WCET (us) |
|--------------------|---------:|----------:|
| Temperature Sensor | 8        | 1105      |
| Tempature Monitor  | 8        | 6922      |
| Log Coordinator    | 9        | 4699      |
| Antenna            | 12       | 10030     |
| Web Monitor        | 4        |   NA      |

![WCET Proof](https://github.com/rkh-98-26/rkh-98-26.github.io/blob/main/WCET.png?raw=true)

Ideal execution runtime loop: 50 ms

Total utilization U = (1105 + 6922 + 4699 +10030)/50000 = 0.455122

RMS L&L: 4(2^(1/4) - 1) = 0.7568
EDF: 1

In theory, this program would be viable under RMS and EDf styles of scheduling, though 
it should be noted that this program isn't really designed around this scheduling as its 
main purpose is to demonstrate pipelining in FreeRTOS.

## Hazard analysis & standard mapping
Removing the Task delay from the Temperature Sensor Task causes the tasks to lose their synchronization 
and the entire program to get shut down and reset by the watchdog timer. The heavy processing of packets 
through the queue causes CPU overuse and task starvation. This is why there must be some delay for the 
producer task of this program specfically.

![Hazard Proof](https://github.com/rkh-98-26/rkh-98-26.github.io/blob/main/Hazard.png?raw=true)

## Graceful degradation
Should the queue be filled, the Temperature Sensor task will simply drop the latest packet 
and wait until the queue has been emptied to at least half capacity before returning to sending 
packets again. This back pressure policy works, but if relied upon, the program will become 
very prone to data bursts and stalls in the pipelining.

## Tailored for
This project is tailored towards sending temperature data from a sensor on a satellite 
over to be processed and sent down to ground control.

The queue allows for continuous deployment from the sensor to to the monitor. The event 
group allows for a single successful data transfer to single a transmission to ground 
control through the antenna, which is itself signaled by a low-latency direct task 
notification.

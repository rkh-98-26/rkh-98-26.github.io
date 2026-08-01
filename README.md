# SAT-RKH-1 — Real-Time Systems Final Capstone

## One sentence
This is a Real Time System simulating a satellite sensing the internal temperature of it's circuitry, sending that data 
over to be processed, and having that data sent over to Ground control on earth.

## Demo
- Video: [Video Link](https://youtu.be/iNfluWkuU1s)
- Live Wokwi: [Wokwi Link](https://wokwi.com/projects/471005401497528321)
- Website: [Website Link](https://rkh-98-26.github.io/)

## Architecture

![architecture](https://github.com/rkh-98-26/rkh-98-26.github.io/blob/main/Final%20Application%20Arhcitecture.png?raw=true)

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

## Additional Final Reflection

If I was given more time to develop this project, I would have naturally tried to integrate all previous Applications into a singular program. In particular, I would have integrate tasks from Application 4, those being the 
solar panels that adjust only when given a slot from a counting semaphore as well as alternating antennas activating based on a mutex. The web monitor would be developed such that it would let the user know which solar panels 
and antennas are active and which are not. From Application 1, I would also simulate the half-duplex data transfer between the satellite and ground control using those antenna tasks, which would also be displayed on the web 
monitor. I would also expand the telemtry data to be a wide array of sensors such as altitude and orbital pathing. I would also be more stringet with task periods and deadlines, as this particular application was not well 
suited for that type of synchronization as it is mostly focused on Pipelining. 

I had a hard time integrating the WCET measurement function into this project because it originally didn't have much material to give a noticable excution time. I had to create my own functions for the sole purpose of 
simulating work for each task and then integrate the WCET function, which caused massive spikes because it wrapped around the ESP_LOG functions, so those had to be removed from the measurement block. I spent the most amount 
of time debugging this to get myself in an acceptable state for running the program.

The most valuable thing I learned from this project likely had to do with the setup for github itself. I learned how to create a website with github pages, fill it will fiels from the assoicated repository, and modify the 
index file to allow for better functionality in accessing the various pieces of data it offers, specifically the project files including its README.


## AI Disclosure

I utilized the AI LLM Google Gemini to aid me in creating and debugging this capstone project as well as the 
associated github pages website: [AI Chat Log](https://gemini.google.com/app/1d257e6d618092e7)sclosure

I utilized the AI LLM Google Gemini to aid me in creating and debugging this capstone project as well as the 
associated github pages website: [AI Chat Log](https://gemini.google.com/app/1d257e6d618092e7)

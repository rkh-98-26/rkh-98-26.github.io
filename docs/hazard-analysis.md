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

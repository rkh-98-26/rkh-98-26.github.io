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

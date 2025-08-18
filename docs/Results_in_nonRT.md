## Performance metrics(non-RT)  
Intel Atom C3808 & Intel X553 NIC

1. Generating 1000 SV80 and Receiving Them Back via 10Gb/s SFP Module

   - Packet Rate: 4 million packets per second (PPS)
   - Data Rate: 3968 Mb/s

   Bus Generator (Core 1):

   | Metric | Min (µs) | Max (µs) | Load % | Wait % |
   |--------|----------|----------|--------|--------|
   | Main   | 94       | 217      | 47.016 | 52.984 |

   Bus Processor (Core 2):

   | Metric | Min (µs) | Max (µs) | Load %  | Wait %  |
   |--------|----------|----------|---------|---------|
   | Main   | 0        | 94       | 70.686  | 29.314  |

   Observations:  
   Stream Integrity: 1-2 samples were lost on some SV-streams after 5 minutes.

2. Generating 100 GOOSE Messages with 10,000 Changes Per Second

   - Packet Rate: 1 million packets per second (PPS)
   - Data Rate: 1560 Mb/s

   Bus Generator (Core 1):

   | Metric | Min (µs) | Max (µs) | Load % | Wait % |
   |--------|----------|----------|--------|--------|
   | Main   |12        | 85       | 15.169 | 84.831 |

   Bus Processor (Core 2):

   | Metric | Min (µs) | Max (µs) | Load %  | Wait %  |
   |--------|----------|----------|---------|---------|
   | Main   | 0        | 63       | 35.541  | 64.459  |

   Observations:  
   Stream Integrity: no messages were lost after 5 minutes.

3. Generating 200 GOOSE Messages with 10,000 Changes Per Second

   - Packet Rate: 2 million packets per second (PPS)
   - Data Rate: 3120 Mb/s

   Bus Generator (Core 1):

   | Metric | Min (µs) | Max (µs) | Load %  | Wait %  |
   |--------|----------|----------|---------|---------|
   | Main   | 27       | 115      | 35.892  | 64.108  |

   Bus Processor (Core 2):

   | Metric | Min (µs) | Max (µs) | Load %  | Wait %  |
   |--------|----------|----------|---------|---------|
   | Main   | 0        | 86       | 72.138  | 27.862  |

   Observations:  
   Stream Integrity: 1-2 GOOSE messages were lost after 5 minutes.

4. Generating 300 GOOSE Messages with 10,000 Changes Per Second

   - Packet Rate: 3 million packets per second (PPS)
   - Data Rate: 4680 Mb/s

   Bus Generator (Core 1):

   | Metric | Min (µs) | Max (µs) | Load %  | Wait %  |
   |--------|----------|----------|---------|---------|
   | Main   | 38       | 145      | 56.655  | 43.345  |

   Bus Processor (Core 2,3,4):

   | Metric | Min (µs) | Max (µs) | Load %  | Wait %  |
   |--------|----------|----------|---------|---------|
   | Main   | 0        | 74       | 53.423  | 46.577  |
   | LCore3 | 0        | 64       | 58.682  | 41.318  |
   | LCore4 | 0        | 97       | 72.205  | 27.795  |

   Observations:  
   Stream Integrity: no messages were lost after 5 minutes.

5. Generating 350 GOOSE Messages with 10,000 Changes Per Second

   - Packet Rate: 3.5 million packets per second (PPS)
   - Data Rate: 5460 Mb/s

   Bus Generator (Core 1):

   | Metric | Min (µs) | Max (µs) | Load %  | Wait %  |
   |--------|----------|----------|---------|---------|
   | Main   | 45       | 160      | 71.306  | 28.694  |

   Bus Processor (Core 2,3,4):

   | Metric | Min (µs) | Max (µs) | Load %  | Wait %  |
   |--------|----------|----------|---------|---------|
   | Main   | 0        | 82       | 38.934  | 61.066  |
   | LCore3 | 0        | 72       | 73.692  | 26.308  |
   | LCore4 | 0        | 109      | 89.894  | 10.106  |

   Observations:  
   Stream Integrity: no messages were lost after 5 minutes.


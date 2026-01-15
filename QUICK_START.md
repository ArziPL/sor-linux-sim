# SOR - Quick Start Guide

## Installation

```bash
cd /home/areczek/sor-linux-sim
make
```

## Basic Usage

### Run default simulation (infinite)
```bash
./sor
```

### Run 10-second simulation
```bash
./sor --duration 10
```

### Customize patient arrival rate
```bash
# Patient every 5 seconds
./sor --duration 30 --interval 5.0

# Patient every 0.5 seconds (fast)
./sor --duration 5 --interval 0.5
```

### Fast simulation (2x speed)
```bash
./sor --duration 10 --speed 2.0
```

### View output
```bash
tail -f sor.log
```

## Common Problems

### Simulation freezes
Press Ctrl+C to stop gracefully.

### Resources not cleaned up
```bash
ipcrm -a
```

### Need to clean rebuild
```bash
make clean
make
```

## Command Reference

```
./sor [OPTIONS]

OPTIONS:
  --duration <sec>   Simulation duration (default: infinite)
  --N <int>         Registration windows (default: 1)
  --K <int>         Waiting room seats (default: 20)
  --speed <float>   Speed multiplier (default: 2.0)
  --interval <sec>  Patient arrival interval in seconds (default: 3.0)
  --seed <int>      Random seed (default: time())
  --help            Show help
```

## Example Scenarios

### Slow, realistic simulation
```bash
./sor --duration 60 --interval 5.0 --speed 1.0
```

### Fast stress test
```bash
./sor --duration 5 --interval 0.1 --speed 5.0
```

### Multiple registration windows
```bash
./sor --duration 20 --N 3 --K 50
```

## Monitoring

### Check active processes
```bash
ps aux | grep sor
```

### View log in real-time
```bash
tail -f sor.log
```

### Count patients
```bash
grep "pojawia się" sor.log | wc -l
```

### Check IPC resources
```bash
ipcs
```

## Notes

- Timestamps in log are relative to simulation start
- Children with guardians occupy 2 waiting room seats
- VIP patients (20% chance) skip registration queue
- All resources are automatically cleaned up on exit

---

For detailed information, see `RAPORT_FINALNY.md`

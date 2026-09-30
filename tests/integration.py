#!/usr/bin/env python3
"""Actual CLI tests with isolated fixtures; no load-dependent assertions."""
import json
import os
from pathlib import Path
import signal
import selectors
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())

class CLI(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.put('proc/stat', 'cpu 100 0 20 880 0 0 0 0\ncpu0 100 0 20 880 0 0 0 0\nprocs_running 1\nprocs_blocked 0\n')
        self.put('proc/meminfo', 'MemTotal: 1000 kB\nMemFree: 1 kB\nMemAvailable: 500 kB\nSwapTotal: 100 kB\nSwapFree: 50 kB\n')
        self.put('proc/loadavg', '0.5 0.2 0.1 1/10 42\n')
        self.put('proc/vmstat', 'pswpin 0\npswpout 0\n')
        self.put('proc/uptime', '1000 500\n')
        self.put('proc/self/mountinfo', '1 0 0:1 / / rw - overlay overlay rw\n2 0 0:2 / /sys/fs/cgroup rw - cgroup2 cgroup rw\n')
        self.put('proc/fixture-capacity/1', '1000 500 500 4096 1000 500')
        self.put('proc/self/cgroup', '0::/\n')
        self.put('sys/fs/cgroup/cpu.max', 'max 100000\n')
        self.put('sys/fs/cgroup/memory.max', 'max\n')
        self.put('sys/fs/cgroup/memory.current', '1024\n')
        self.put('sys/fs/cgroup/cpuset.cpus.effective', '0\n')
        self.put('proc/diskstats', '8 0 sda 1 0 2 3 4 0 5 6 0 7 8\n')
        for resource in ('cpu', 'memory', 'io'):
            psi = 'some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n'
            self.put('proc/pressure/' + resource, psi)
            self.put('sys/fs/cgroup/' + resource + '.pressure', psi)
        self.process('worker')
    def tearDown(self):
        self.tmp.cleanup()
    def put(self, path, data):
        p = self.root / path
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(data)
    def process(self, name, state='R', start=1, user=10):
        fields = ['0'] * 22
        fields[0], fields[11], fields[12], fields[17], fields[19], fields[21] = state, str(user), '5', '1', str(start), '10'
        self.put('proc/42/stat', '42 (' + name + ') ' + ' '.join(fields))
        self.put('proc/42/io', 'read_bytes: 100\nwrite_bytes: 200\nrchar: 300\nwchar: 400\n')
    def command(self, *args):
        return [BINARY, *args, '--fixture-root', str(self.root)]
    def run_cli(self, *args, code=0):
        p = subprocess.run(self.command(*args), capture_output=True, timeout=10)
        self.assertEqual(p.returncode, code, p.stderr.decode(errors='replace'))
        return p
    def report(self, *args, code=0):
        p = self.run_cli(*args, '--format', 'json', code=code)
        self.assertEqual(p.stderr, b'')
        return json.loads(p.stdout)
    def baseline(self, p):
        """Wait for the flushed first sample; never assume instrumented startup time."""
        prefix = b''
        deadline = time.monotonic() + 5
        try:
            with selectors.DefaultSelector() as sel:
                sel.register(p.stdout, selectors.EVENT_READ)
                while time.monotonic() < deadline:
                    if not sel.select(0.05):
                        continue
                    chunk = os.read(p.stdout.fileno(), 65536)
                    if not chunk:
                        break
                    prefix += chunk
                    text = prefix.decode('ascii')
                    marker = '"samples":['
                    if marker in text:
                        try:
                            first, _ = json.JSONDecoder().raw_decode(text.split(marker, 1)[1])
                            return prefix, first
                        except json.JSONDecodeError:
                            pass
            self.fail('first sample was not produced within the bounded deadline')
        except BaseException:
            p.kill()
            p.communicate(timeout=3)
            raise

    def test_snapshot(self):
        data = self.report('snapshot')
        self.assertEqual(len(data['samples']), 1)
        self.assertNotIn('cpu.total.busy_percent', data['samples'][0]['derived'])
    def test_sample(self):
        self.assertEqual(len(self.report('sample', '--interval', '0.05', '--samples', '3')['samples']), 3)
    def test_duration(self):
        data = self.report('sample', '--interval', '0.05', '--duration', '0.13')
        self.assertGreaterEqual(len(data['samples']), 2)
        self.assertGreaterEqual(data['samples'][-1]['monotonic_seconds']-data['samples'][0]['monotonic_seconds'], 0.12)
    def test_processes(self):
        data = self.report('processes', '--interval', '0.05')
        self.assertIn('process.42.cpu_percent_one_core', data['samples'][1]['derived'])
    def test_summary(self):
        self.assertEqual(len(self.report('summary', '--interval', '0.05')['samples']), 2)
    def test_markdown(self):
        self.assertTrue(self.run_cli('--format', 'markdown').stdout.startswith(b'## why-slow'))
    def test_explain(self):
        self.assertIn(b'Raw observations', self.run_cli('--explain').stdout)
    def test_version(self):
        p = subprocess.run([BINARY, '--version'], capture_output=True, timeout=5)
        self.assertEqual(p.stdout, b'why-slow 1.0.0\n')
    def test_help(self):
        p = subprocess.run([BINARY, '--help'], capture_output=True, timeout=5)
        self.assertEqual(p.returncode, 0)
        self.assertIn(b'Usage:', p.stdout)
    def test_invalid_options(self):
        for args in [('sample','--interval','0'), ('--bogus','x'), ('sample','--samples','-1'), ('sample','--interval','nan'), ('--format','yaml')]:
            with self.subTest(args=args):
                self.assertEqual(self.run_cli(*args, code=2).stdout, b'')
    def test_conflicting_options(self):
        self.run_cli('sample','--samples','3','--duration','1',code=2)
    def test_snapshot_sampling_rejected(self):
        self.run_cli('snapshot','--interval','1',code=2)
    def test_max_span(self):
        self.run_cli('sample','--samples','1000','--interval','60',code=2)
    def test_missing_everything(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = subprocess.run([BINARY,'--fixture-root',tmp,'--format','json'],capture_output=True,timeout=10)
            self.assertEqual(p.returncode,1)
            self.assertIn('cpu',json.loads(p.stdout)['samples'][0]['unavailable'])
    def test_missing_optional(self):
        (self.root/'proc/pressure/io').unlink()
        data=self.report()
        self.assertIn('psi.io',data['samples'][0]['unavailable'])
    def test_malformed(self):
        self.put('proc/stat','cpu broken\n')
        self.put('proc/pressure/cpu','some avg10=nan\n')
        self.put('proc/42/stat','42 (bad) junk')
        self.assertIn('cpu',self.report()['samples'][0]['unavailable'])
    def test_no_indicator(self):
        findings=self.report()['samples'][0]['findings']
        self.assertTrue(any(f['id']=='evidence.no_indicator' for f in findings))
    def test_multiple_indicators(self):
        self.put('proc/meminfo','MemTotal: 1000 kB\nMemAvailable: 1 kB\n')
        self.put('proc/fixture-capacity/1','1000 1 1 4096 1000 1')
        findings=self.report()['samples'][0]['findings']
        self.assertGreaterEqual(sum(f['classification']=='INDICATOR' for f in findings),2)
    def test_control_and_json_escaping(self):
        self.process('x\n\x1b\"\\y')
        p=self.run_cli('--format','json')
        self.assertNotIn(b'\x1b',p.stdout)
        name=json.loads(p.stdout)['samples'][0]['observations']['process.42.name']
        self.assertEqual(name,'x??"\\y')
    def test_no_sensitive_arguments(self):
        self.put('proc/42/cmdline','worker\x00password=secret-token\x00')
        self.put('proc/42/environ','TOKEN=super-secret\x00')
        self.assertNotIn(b'secret',self.run_cli('--explain').stdout)
    def test_no_mount_paths(self):
        self.put('proc/self/mountinfo','1 0 8:1 / /home/private-person rw - ext4 /dev/private-disk rw\n')
        output=self.run_cli('--format','json').stdout
        self.assertNotIn(b'private-person',output)
        self.assertNotIn(b'private-disk',output)
    def test_fifo_nonblocking(self):
        p=self.root/'proc/stat';p.unlink();os.mkfifo(p)
        start=time.monotonic();data=self.report()
        self.assertLess(time.monotonic()-start,3)
        self.assertIn('cpu',data['samples'][0]['unavailable'])
    def test_oversized_file(self):
        self.put('proc/stat','x'*1048577)
        self.assertIn('cpu',self.report()['samples'][0]['unavailable'])
    def test_symlink_refused(self):
        p=self.root/'proc/stat';p.unlink();p.symlink_to('/etc/passwd')
        self.assertIn('cpu',self.report()['samples'][0]['unavailable'])
    def test_io_permission_equivalent(self):
        # Missing per-process I/O uses the same unavailable path as EACCES.
        (self.root/'proc/42/io').unlink()
        self.assertIn('process.io',self.report()['samples'][0]['unavailable'])
    def test_sigint(self):
        p=subprocess.Popen(self.command('sample','--interval','0.2','--samples','30','--format','json'),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        prefix, _ = self.baseline(p)
        p.send_signal(signal.SIGINT)
        out,err=p.communicate(timeout=8)
        self.assertEqual(p.returncode,130,err)
        self.assertTrue(json.loads(prefix + out)['interrupted'])
    def test_sigterm(self):
        p=subprocess.Popen(self.command('sample','--interval','0.2','--samples','30','--format','json'),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        prefix, _ = self.baseline(p)
        p.terminate();out,err=p.communicate(timeout=8)
        self.assertEqual(p.returncode,143,err)
        self.assertTrue(json.loads(prefix + out)['interrupted'])
    def test_broken_pipe(self):
        rd,wr=os.pipe();os.close(rd)
        p=subprocess.Popen(self.command('--format','json'),stdout=wr,stderr=subprocess.PIPE)
        os.close(wr);_,err=p.communicate(timeout=8)
        self.assertIn(p.returncode,(141,3),err)
    def test_readonly_fixture(self):
        before={str(p):p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        self.report('sample','--interval','0.05','--samples','2')
        after={str(p):p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        self.assertEqual(before,after)
    def test_fixture_not_host_capacity(self):
        self.put('proc/fixture-capacity/1','100 50 25 4096 0 0')
        self.assertEqual(self.report()['samples'][0]['derived']['filesystem.1.available_percent'],25)
    def test_cgroup_constraints(self):
        self.put('sys/fs/cgroup/cpu.max','25000 100000')
        self.put('sys/fs/cgroup/memory.max','2048')
        self.put('sys/fs/cgroup/cpuset.cpus.effective','0-3')
        obs=self.report()['samples'][0]['observations']
        self.assertEqual(float(obs['cgroup.visible_quota_cpus']),0.25)
        self.assertEqual(obs['cgroup.cpuset_count'],'4')
    def test_cgroup_v1(self):
        self.put('proc/self/cgroup','2:cpu:/group')
        self.assertEqual(self.report()['samples'][0]['observations']['cgroup.mode'],'v1 (not interpreted)')
    def test_thermal_no_throttle(self):
        self.put('sys/class/thermal/thermal_zone0/temp','100000')
        ids=[f['id'] for f in self.report()['samples'][0]['findings']]
        self.assertIn('thermal.context',ids)
        self.assertNotIn('thermal.throttle_counter',ids)
    def test_process_zombie(self):
        self.process('zombie','Z')
        self.assertEqual(self.report()['samples'][0]['observations']['process.Z_count'],'1')
    def test_top_limit(self):
        output=self.run_cli('--top','1').stdout
        self.assertIn(b'process.42.',output)

    def test_fixture_directory_symlink_refused(self):
        # Even a readable external directory is not followed in fixture mode.
        with tempfile.TemporaryDirectory() as outside:
            Path(outside, 'stat').write_text((self.root/'proc/42/stat').read_text())
            (self.root/'proc/42/stat').unlink()
            (self.root/'proc/42/io').unlink()
            (self.root/'proc/42').rmdir()
            (self.root/'proc/42').symlink_to(outside)
            self.assertNotIn('process.42.name',self.report()['samples'][0]['observations'])
    def test_duration_sample_cap(self):
        self.run_cli('sample','--duration','3600','--interval','0.05',code=2)
    def test_changed_process_identity(self):
        p=subprocess.Popen(self.command('sample','--interval','1','--samples','2','--format','json'),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        prefix, first = self.baseline(p)
        self.assertEqual(first['observations']['process.42.start_ticks'], '1')
        self.process('replacement',start=100,user=500)
        out,err=p.communicate(timeout=8)
        self.assertEqual(p.returncode,0,err)
        data=json.loads(prefix + out)
        self.assertEqual(data['samples'][1]['observations']['process.42.start_ticks'], '100')
        self.assertNotIn('process.42.cpu_percent_one_core',data['samples'][1]['derived'])
    def test_counter_reset(self):
        p=subprocess.Popen(self.command('sample','--interval','1','--samples','2','--format','json'),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        prefix, first = self.baseline(p)
        self.assertEqual(first['observations']['cpu.total.user'], '100')
        self.put('proc/stat','cpu 1 0 0 0 0 0 0 0\ncpu0 1 0 0 0 0 0 0 0\n')
        out,err=p.communicate(timeout=8)
        self.assertEqual(p.returncode,0,err)
        self.assertNotIn('cpu.total.busy_percent',json.loads(prefix + out)['samples'][1]['derived'])

    def test_repeated_sampling(self):
        data = self.report('sample', '--samples', '100', '--interval', '0.05')
        self.assertEqual(len(data['samples']), 100)
        self.assertFalse(data['interrupted'])
        self.assertTrue(all('collection' not in s['unavailable'] for s in data['samples']))

    def test_zero_memory_is_not_core_data(self):
        self.put('proc/stat', 'malformed')
        self.put('proc/meminfo', 'MemTotal: 0 kB\n')
        self.put('proc/42/stat', 'malformed')
        self.report(code=1)

if __name__=='__main__':
    unittest.main(verbosity=2)

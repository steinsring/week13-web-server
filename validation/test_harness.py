import asyncio
import os
import unittest
from fixtures import SMALL
from load import Client, Stats, process_sample, workload


class Writer:
    def write(self, value): pass
    async def drain(self): pass


class HarnessTests(unittest.IsolatedAsyncioTestCase):
    async def test_body_validation_rejects_same_length_corruption(self):
        client = Client(1, False)
        client.writer = Writer()
        client.reader = asyncio.StreamReader()
        client.reader.feed_data(b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 4096\r\n\r\n'
                                + SMALL[:-1]+bytes([SMALL[-1]^1]))
        result = await client.execute(workload('static', 0), Stats())
        self.assertEqual(result[:2], (False, 'body_mismatch'))

    async def test_duplicate_framing_rejected(self):
        client = Client(1, False)
        client.writer = Writer(); client.reader = asyncio.StreamReader()
        client.reader.feed_data(b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 4096\r\nContent-Length: 4096\r\n\r\n'+SMALL)
        self.assertFalse((await client.execute(workload('static', 0), Stats()))[0])

    async def test_histogram_rank_cutoff_and_reuse(self):
        stats = Stats(); stats.issued = 3
        stats.finish(.00101, True, '', True)
        stats.finish(.00201, False, 'http_503', True)
        stats.finish(.05001, True, '', False)
        value = stats.summary(30)
        self.assertEqual(value['incomplete_at_window_end'], 1)
        self.assertEqual(value['success_within_window'], 1)
        self.assertEqual(value['latency_ms']['p99'], {'lower_ms': 50.0, 'upper_ms': 50.1})
        self.assertEqual(value['errors'], {'http_503': 1})

    async def test_error_body_semantics(self):
        for body, expected in [(b'Service Unavailable', 'http_503'), (b'x'*19, 'error_body_mismatch')]:
            client=Client(1,False);client.writer=Writer();client.reader=asyncio.StreamReader()
            client.reader.feed_data(b'HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nContent-Length: 19\r\n\r\n'+body)
            self.assertEqual((await client.execute(workload('static',0),Stats()))[1],expected)

    async def test_metrics_and_workload_mix(self):
        self.assertGreater(process_sample(os.getpid())['rss_kib'], 0)
        choices = [workload('mixed', i)[1] for i in range(4)]
        self.assertEqual(choices, ['/small.txt', '/api/get', '/large.bin', '/api/post'])


if __name__ == '__main__': unittest.main(verbosity=2)

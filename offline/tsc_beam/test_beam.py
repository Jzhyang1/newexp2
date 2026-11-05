"""Checks for bucket encoding, beam search ordering, and end-to-end training."""

import unittest

import torch

from offline.tsc_beam import __main__ as beam


class BeamTests(unittest.TestCase):
    def test_encode_decode_round_trip(self):
        offsets = torch.tensor([0, 1 << 40, (1 << 50) - 1024, 123456789 << 10])
        for levels in (4, 5):
            leaf = (1 << (50 - 10 * levels)) - 1
            self.assertTrue(torch.equal(beam.decode(beam.encode(offsets, levels)), offsets & ~leaf))
        self.assertEqual(beam.encode(torch.tensor(3 << 40), 2).tolist(), [3, 0])

    def test_beam_matches_exhaustive_search_when_beam_is_wide(self):
        torch.manual_seed(0)
        net = beam.BeamClassifier(2, 4, 8).eval()
        history = torch.randint(0, beam.BUCKETS, (3, 5, 2))
        paths, scores = beam.beam_search(net, history, beam.BUCKETS * 2)
        state = net.encode_history(history)
        first = net.heads[0](state).log_softmax(-1)
        every = torch.arange(beam.BUCKETS)
        for row in range(3):
            second = net.heads[1](net.advance(1, state[row].expand(beam.BUCKETS, -1), every)).log_softmax(-1)
            total = first[row][:, None] + second
            best = total.flatten().argmax().item()
            self.assertEqual(paths[row, 0].tolist(), [best // beam.BUCKETS, best % beam.BUCKETS])
            self.assertAlmostEqual(scores[row, 0].item(), total.flatten()[best].item(), places=4)
        self.assertTrue((scores[:, :-1] >= scores[:, 1:]).all())

    def test_learns_repeating_pattern_and_predicts(self):
        cycle = [(i * (7 << 40) + (i << 20), 512) for i in range(4)]
        trace = cycle * 40
        model = beam.BeamModel(2, 1, 4, 4, trace[:128], trace[128:], 40, 16, 8, 32)
        self.assertGreater(model.test_metrics["recall"], 0.99)
        self.assertEqual(model.predict(trace[:2], 1)[0], (trace[2][0], 1024))
        self.assertEqual(len(model.predict(trace[:2], 3)), 3)
        self.assertEqual(model.predict(trace[:1], 1), [])

    def test_rejects_out_of_disk_offsets(self):
        with self.assertRaises(ValueError):
            beam.BeamModel(1, 1, 4, 2, [(0, 1), (1 << 50, 1)], [], 1, 1, 4, 4)


if __name__ == "__main__":
    unittest.main()

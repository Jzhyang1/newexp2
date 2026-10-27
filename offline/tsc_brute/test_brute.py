"""Behavior checks for page classification, scoring, and the serving policy."""

import contextlib
import io
import json
import math
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import torch

from offline.serve import serve
from offline.tsc_brute import __main__ as brute


class BruteTests(unittest.TestCase):
    def test_main_reuses_cached_model_and_retrains_when_args_change(self):
        train = [(i % 4 * brute.PAGE_BYTES, 512) for i in range(8)]
        argv = ["tsc_brute", "--training", "trace.csv", "--epochs", "1",
                "--hide-loss-curve", "--dry-run"]
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(brute, "CACHE_PATH", Path(directory) / "training_cache.pt"), \
                patch.object(brute, "NUM_PAGES", 4), \
                patch.object(brute, "load_training", return_value=train) as load_training, \
                patch("sys.argv", argv), contextlib.redirect_stderr(io.StringIO()):
            brute.main()
            load_training.assert_called_once_with("trace.csv")
            checkpoint = torch.load(brute.CACHE_PATH, weights_only=True)
            self.assertEqual(checkpoint["args"]["epochs"], 1)
            self.assertEqual(len(checkpoint["model"]["train_losses"]), 1)

            # A hit must skip both CSV loading and the training constructor.
            load_training.reset_mock()
            with patch.object(brute.BruteModel, "__init__", side_effect=AssertionError("retrained")):
                brute.main()
                args = brute.argparse.Namespace(**checkpoint["args"])
                restored = brute.load_or_train(args)
            load_training.assert_not_called()
            self.assertEqual(restored.train_losses, checkpoint["model"]["train_losses"])
            self.assertEqual(restored.test_regression_losses,
                             checkpoint["model"]["test_regression_losses"])
            for name, value in restored.network.state_dict().items():
                self.assertTrue(torch.equal(value, checkpoint["state_dict"][name]))
            self.assertIsNotNone(restored.probabilities(train))

            argv[argv.index("--epochs") + 1] = "2"
            brute.main()
            load_training.assert_called_once_with("trace.csv")
            checkpoint = torch.load(brute.CACHE_PATH, weights_only=True)
            self.assertEqual(checkpoint["args"]["epochs"], 2)
            self.assertEqual(len(checkpoint["model"]["train_losses"]), 2)

            # An interrupted or damaged checkpoint is replaced by fresh training.
            brute.CACHE_PATH.write_bytes(b"incomplete checkpoint")
            load_training.reset_mock()
            brute.main()
            load_training.assert_called_once_with("trace.csv")
            self.assertEqual(torch.load(brute.CACHE_PATH, weights_only=True)["args"],
                             checkpoint["args"])

    def test_page_coverage_and_disk_edges(self):
        self.assertEqual(brute.NUM_PAGES * brute.PAGE_BYTES, brute.DISK_BYTES)
        self.assertEqual(brute.page_bounds((0, 512)), (0, 0))
        self.assertEqual(brute.page_bounds((3584, 1024)), (0, 1))
        self.assertEqual(brute.page_bounds((brute.DISK_BYTES - 512, 512)),
                         (brute.NUM_PAGES - 1, brute.NUM_PAGES - 1))
        for pair in [(-512, 512), (0, 0), (brute.DISK_BYTES, 512),
                     (brute.DISK_BYTES - 512, 1024)]:
            with self.assertRaises(ValueError):
                brute.page_bounds(pair)

    def test_classification_marks_all_pages_of_next_request(self):
        logits = torch.tensor([[1., -2., 3., 4.], [-1., 2., -3., 0.]])
        bounds = torch.tensor([[1, 2], [0, 0]])
        expected = torch.nn.functional.binary_cross_entropy_with_logits(
            logits, torch.tensor([[0., 1., 1., 0.], [1., 0., 0., 0.]]))
        self.assertTrue(torch.allclose(brute.classification_loss(logits, bounds), expected))

    def test_peek_adds_weighted_loss_on_request_after_next(self):
        logits = torch.tensor([[1., -2., 3., 4.], [-1., 2., -3., 0.]])
        bounds = torch.tensor([[1, 2, 0, 0], [0, 0, 3, 3]])
        bce = torch.nn.functional.binary_cross_entropy_with_logits
        current = bce(logits, torch.tensor([[0., 1., 1., 0.], [1., 0., 0., 0.]]))
        peek = bce(logits, torch.tensor([[1., 0., 0., 0.], [0., 0., 0., 1.]]))
        self.assertTrue(torch.allclose(brute.classification_loss(logits, bounds), current))
        self.assertTrue(torch.allclose(brute.classification_loss(logits, bounds, 0.5), current + 0.5 * peek))

    def test_dataset_peeks_at_following_request_and_repeats_at_the_tail(self):
        model = brute.BruteModel.__new__(brute.BruteModel)
        model.history, model.niceness = 1, 1
        pairs = [(0, 512), (brute.PAGE_BYTES, 512), (2 * brute.PAGE_BYTES, 512)]
        self.assertEqual(model._dataset(pairs).tensors[1].tolist(), [[1, 1, 2, 2], [2, 2, 2, 2]])

    def test_niceness_changes_only_regression_scoring(self):
        class FixedNetwork(torch.nn.Module):
            def forward(self, values):
                logits = torch.zeros(len(values), 4)
                logits[:, 2] = 4
                return logits

        pairs = [(0, 512), (0, 512), (8192, 512)]
        model = brute.BruteModel.__new__(brute.BruteModel)
        model.history, model.batch_size = 1, 2
        model.network = FixedNetwork()
        model.niceness, model.peek_alpha = 1, 0.5
        strict = model._evaluate(model._dataset(pairs))
        model.niceness = 2
        nice = model._evaluate(model._dataset(pairs))
        self.assertEqual(strict[0], nice[0])
        self.assertAlmostEqual(strict[1], 8192 / math.sqrt(2))
        self.assertEqual(nice[1], 0)

    def test_training_and_short_histories(self):
        train = [(i % 4 * brute.PAGE_BYTES, 512) for i in range(8)]
        with patch.object(brute, "NUM_PAGES", 4):
            model = brute.BruteModel(2, 2, train, train, 2, batch_size=2, alpha=0)
            self.assertEqual(len(model.train_losses), 2)
            self.assertTrue(all(math.isfinite(x) for x in model.train_losses))
            self.assertEqual(model.network(torch.zeros(2, 2, 7)).shape, (2, 4))
            self.assertEqual(model.predict([], 1), [])
            self.assertEqual(model.predict(train, 0), [])
            with self.assertRaises(ValueError):
                model.predict(train, -1)
            probability = model.probabilities(train)
            self.assertTrue(bool(((probability >= 0) & (probability <= 1)).all()))
            self.assertEqual(len(model.predict(train, 16)), 1)
            self.assertTrue(math.isnan(model._evaluate(model._dataset([]))[0]))

    def test_serve_converts_blocks_to_bytes_and_back(self):
        class FakeModel:
            def predict(self, previous, count):
                self.previous, self.count = previous, count
                return [(4096, 8192)]

        model = FakeModel()
        requests = [
            {"type": "get", "prev_accesses": [[8, 1]], "next_n": 16},
            {"type": "put", "actual_accesses": [8, 1]},
            {"type": "kill"},
        ]
        output = io.StringIO()
        with patch("sys.stdin", io.StringIO("\n".join(map(json.dumps, requests)))), \
                contextlib.redirect_stdout(output):
            serve(model.predict)
        self.assertEqual(model.previous, [(4096, 512)])
        self.assertEqual(model.count, 16)
        self.assertEqual([json.loads(line) for line in output.getvalue().splitlines()],
                         [[[8, 16]], []])


if __name__ == "__main__":
    torch.set_num_threads(1)
    unittest.main()

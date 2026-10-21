"""Plot training loss histories for offline models."""

from __future__ import annotations

from collections.abc import Sequence


def show_loss_curve(train_losses: Sequence[float], test_losses: Sequence[float], output_path: str | None = None) -> None:
    """Display a training-loss curve or save it when an output path is given."""
    import matplotlib
    import matplotlib.pyplot as plt
    matplotlib.use('TkAgg')

    figure, axis = plt.subplots()
    axis.plot(range(1, len(train_losses) + 1), train_losses, 'r')
    axis.plot(range(1, len(test_losses) + 1), test_losses, 'b')
    axis.set_xlabel("Epoch")
    axis.set_ylabel("Training loss")
    axis.set_title("Training loss")
    axis.grid(True, alpha=0.3)
    figure.tight_layout()
    if output_path is None:
        plt.show()
    else:
        figure.savefig(output_path)
    plt.close(figure)

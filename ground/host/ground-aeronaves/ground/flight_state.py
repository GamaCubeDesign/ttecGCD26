"""Fases com histerese e persistência no tempo da evidência."""

from .temporal import vertical_features


class FlightStateEstimator:
    def __init__(self, persistence_s=10):
        self.persistence_s = persistence_s
        self.state = "UNKNOWN"
        self.pending = None
        self.since = None
        self.last_evidence = None

    def update(self, snapshot, vertical):
        if snapshot is None or vertical["conflict"]:
            self.state, self.pending = "UNKNOWN", None
            return self.state
        if snapshot["fields"].get("on_ground", {}).get("value") == 1:
            self.state, self.pending = "GROUND", None
            return self.state
        rate, time = vertical["value_fpm"], vertical["evidence_ns"]
        if rate is None or time is None:
            self.state, self.pending = "UNKNOWN", None
            return self.state
        if self.last_evidence is not None and time - self.last_evidence > 60e9:
            self.state, self.pending = "UNKNOWN", None
        if self.last_evidence is not None and time <= self.last_evidence:
            return self.state
        self.last_evidence = time
        threshold = 150 if self.state in ("CLIMB", "DESCENT") else 300
        if rate > (threshold if self.state == "CLIMB" else 300):
            target = "CLIMB"
        elif rate < -(threshold if self.state == "DESCENT" else 300):
            target = "DESCENT"
        else:
            target = "LEVEL"
        if target == self.state:
            self.pending = None
        elif self.pending != target:
            self.pending, self.since = target, time
        elif (time - self.since) / 1e9 >= self.persistence_s:
            self.state, self.pending = target, None
        return self.state


def classify(observations, snapshot):
    """Compatibilidade para consumidores sem estado persistente."""
    model = FlightStateEstimator()
    for index, obs in enumerate(observations):
        model.update(
            snapshot, vertical_features(observations[: index + 1], obs.event_ns)
        )
    return model.state

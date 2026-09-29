from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class Observation:
    icao: str
    event_ns: int
    received_ns: int
    timestamp_source: str
    fields: dict[str, Any]
    raw: str

    @property
    def has_position(self) -> bool:
        return self.fields.get('lat') is not None and self.fields.get('lon') is not None

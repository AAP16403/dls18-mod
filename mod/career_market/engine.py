"""Offline reference engine for the DLS18 all-club career transfer market.

This module captures the market rules and transaction invariants in a portable
form. It is not loaded by the Android game; native save, roster, UI, and turn
hooks still have to be integrated before these rules affect an APK.
"""

from __future__ import annotations

import hashlib
import json
import math
import os
import tempfile
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Iterable, Mapping


SCHEMA_VERSION = 2
PREVIOUS_SCHEMA_VERSION = 1
WINDOW_TURNS = 2
MAX_NEGOTIATION_ROUNDS = 3
MAX_CONTRACT_YEARS = 5
AI_CONTRACT_RENEWAL_LIMIT = 3
AI_CONTRACT_RENEWAL_YEARS = 3
ACTIVE_STATUSES = {
    "seller_response",
    "buyer_fee_response",
    "buyer_player_response",
}
TERMINAL_STATUSES = {"completed", "rejected", "withdrawn", "expired"}

POSITION_GROUPS = {
    "GK": "GK",
    "G": "GK",
    "GOALKEEPER": "GK",
    "DEF": "DEF",
    "DF": "DEF",
    "CB": "DEF",
    "LB": "DEF",
    "RB": "DEF",
    "LWB": "DEF",
    "RWB": "DEF",
    "MID": "MID",
    "MF": "MID",
    "CM": "MID",
    "CDM": "MID",
    "CAM": "MID",
    "LM": "MID",
    "RM": "MID",
    "FWD": "FWD",
    "FW": "FWD",
    "ST": "FWD",
    "CF": "FWD",
    "LW": "FWD",
    "RW": "FWD",
    "ATT": "FWD",
}

BUYER_POSITION_TARGETS = {"GK": 2, "DEF": 6, "MID": 6, "FWD": 4}
SELLER_POSITION_FLOORS = {"GK": 1, "DEF": 3, "MID": 3, "FWD": 2}


def position_group(position: str) -> str:
    """Map common DLS/FIFA position labels to a broad squad role."""
    normalized = position.strip().upper()
    return POSITION_GROUPS.get(normalized, normalized or "UNK")


@dataclass
class Player:
    id: str
    name: str
    club_id: str
    position: str
    age: int
    rating: int
    market_value: int
    annual_wage: int
    contract_years: int = 2
    listed_for_sale: bool = False

    def __post_init__(self) -> None:
        if not self.id or not self.name or not self.club_id:
            raise ValueError("Player id, name, and club_id are required")
        if not 15 <= self.age <= 50:
            raise ValueError(f"Invalid player age for {self.id}: {self.age}")
        if not 1 <= self.rating <= 100:
            raise ValueError(f"Invalid player rating for {self.id}: {self.rating}")
        if min(self.market_value, self.annual_wage, self.contract_years) < 0:
            raise ValueError(f"Player financial values cannot be negative: {self.id}")
        if self.contract_years > MAX_CONTRACT_YEARS:
            raise ValueError(f"Contract exceeds {MAX_CONTRACT_YEARS} years: {self.id}")


@dataclass
class Club:
    id: str
    name: str
    prestige: int = 50
    cash: int = 0
    transfer_budget: int = 0
    annual_wage_budget: int = 0
    payroll: int = 0
    revenue_per_turn: int = 0
    min_squad_size: int = 15
    max_squad_size: int = 30

    def __post_init__(self) -> None:
        if not self.id or not self.name:
            raise ValueError("Club id and name are required")
        if not 0 <= self.prestige <= 100:
            raise ValueError(f"Club prestige must be 0..100: {self.id}")
        if min(
            self.cash,
            self.transfer_budget,
            self.annual_wage_budget,
            self.payroll,
            self.revenue_per_turn,
        ) < 0:
            raise ValueError(f"Club economy values cannot be negative: {self.id}")
        if self.min_squad_size < 1 or self.max_squad_size < self.min_squad_size:
            raise ValueError(f"Invalid squad limits for {self.id}")


@dataclass
class Offer:
    id: str
    player_id: str
    buyer_id: str
    seller_id: str
    fee: int
    annual_wage: int
    contract_years: int
    status: str
    season: int
    turn: int
    window_key: str
    rounds: int = 0
    counter_fee: int | None = None
    counter_wage: int | None = None
    player_countered: bool = False
    user_wage_countered: bool = False
    message: str = ""


@dataclass
class TransferRecord:
    offer_id: str
    player_id: str
    player_name: str
    buyer_id: str
    seller_id: str
    fee: int
    annual_wage: int
    contract_years: int
    season: int
    turn: int
    negotiation_rounds: int


@dataclass
class FinanceEntry:
    id: str
    club_id: str
    season: int
    turn: int
    category: str
    cash_delta: int = 0
    transfer_budget_delta: int = 0
    payroll_delta: int = 0
    note: str = ""


@dataclass
class CareerMarket:
    """Persistent market state for one career save slot."""

    save_slot: str
    user_club_id: str
    season: int
    turn: int
    season_turns: int
    clubs: dict[str, Club]
    players: dict[str, Player]
    offers: dict[str, Offer] = field(default_factory=dict)
    history: list[TransferRecord] = field(default_factory=list)
    finance_ledger: list[FinanceEntry] = field(default_factory=list)
    # Activity is keyed by season/window and stores club IDs for each role.
    window_activity: dict[str, dict[str, list[str]]] = field(default_factory=dict)
    schema_version: int = SCHEMA_VERSION

    @classmethod
    def new(
        cls,
        *,
        save_slot: str,
        user_club_id: str,
        clubs: Iterable[Club],
        players: Iterable[Player],
        season: int = 1,
        season_turns: int = 38,
    ) -> "CareerMarket":
        club_rows = list(clubs)
        player_rows = list(players)
        club_map = {club.id: club for club in club_rows}
        player_map = {player.id: player for player in player_rows}
        if not save_slot:
            raise ValueError("save_slot is required")
        if season_turns < 4:
            raise ValueError("A season needs at least four turns for two windows")
        if len(club_map) == 0 or len(player_map) == 0:
            raise ValueError("At least one club and one player are required")
        if len(club_map) != len(club_rows):
            raise ValueError("Club IDs must be unique")
        if len(player_map) != len(player_rows):
            raise ValueError("Player IDs must be unique")
        state = cls(
            save_slot=save_slot,
            user_club_id=user_club_id,
            season=season,
            turn=0,
            season_turns=season_turns,
            clubs=club_map,
            players=player_map,
        )
        if user_club_id not in club_map:
            raise ValueError(f"Unknown user club: {user_club_id}")
        state._seed_missing_economies()
        state._validate()
        return state

    @property
    def midseason_window_start(self) -> int:
        # For a 38-turn season this opens on turns 18 and 19 (zero based).
        return max(2, min(self.season_turns - WINDOW_TURNS, self.season_turns // 2 - 1))

    @property
    def current_window_number(self) -> int | None:
        if 0 <= self.turn < WINDOW_TURNS:
            return 1
        start = self.midseason_window_start
        if start <= self.turn < start + WINDOW_TURNS:
            return 2
        return None

    @property
    def current_window_key(self) -> str | None:
        number = self.current_window_number
        return f"s{self.season}-w{number}" if number is not None else None

    @property
    def window_label(self) -> str:
        return {
            1: "Preseason",
            2: "Midseason",
        }.get(self.current_window_number, "Closed")

    def players_for_club(self, club_id: str) -> list[Player]:
        self._require_club(club_id)
        return [player for player in self.players.values() if player.club_id == club_id]

    def inbox(self) -> list[Offer]:
        return sorted(
            (
                offer
                for offer in self.offers.values()
                if offer.status == "seller_response" and offer.seller_id == self.user_club_id
            ),
            key=lambda offer: (offer.season, offer.turn, offer.id),
        )

    def finance_report(self, club_id: str) -> dict[str, int | str]:
        club = self._require_club(club_id)
        return {
            "club": club.name,
            "cash": club.cash,
            "transfer_budget": club.transfer_budget,
            "annual_wage_budget": club.annual_wage_budget,
            "payroll": club.payroll,
            "wage_room": max(0, club.annual_wage_budget - club.payroll),
            "squad_size": len(self.players_for_club(club_id)),
        }

    def club_financial_history(self, club_id: str) -> list[FinanceEntry]:
        self._require_club(club_id)
        return [entry for entry in self.finance_ledger if entry.club_id == club_id]

    def search_market(
        self,
        *,
        query: str = "",
        position: str | None = None,
        club_id: str | None = None,
        listed_only: bool = False,
    ) -> list[dict[str, Any]]:
        """Return all-club player cards, with estimates in native value units."""
        needle = query.strip().casefold()
        wanted_group = position_group(position) if position else None
        rows: list[dict[str, Any]] = []
        for player in self.players.values():
            if club_id and player.club_id != club_id:
                continue
            if wanted_group and position_group(player.position) != wanted_group:
                continue
            if listed_only and not player.listed_for_sale:
                continue
            if needle and needle not in player.name.casefold():
                continue
            club = self.clubs[player.club_id]
            if self._has_active_player_offer(player.id):
                availability = "Negotiating"
            elif self.current_window_key is None:
                availability = "Window closed"
            else:
                availability = "Available"
            rows.append(
                {
                    "player_id": player.id,
                    "name": player.name,
                    "club_id": club.id,
                    "club": club.name,
                    "position": player.position,
                    "age": player.age,
                    "rating": player.rating,
                    "estimated_value": self.player_value(player.id),
                    "listed_for_sale": player.listed_for_sale,
                    "availability": availability,
                }
            )
        return sorted(rows, key=lambda row: (-row["rating"], row["name"].casefold()))

    def player_value(self, player_id: str, buyer_id: str | None = None) -> int:
        player = self._require_player(player_id)
        value = max(1, player.market_value)
        age_factor = 1.18 if player.age <= 20 else 1.10 if player.age <= 23 else 1.0
        if player.age >= 33:
            age_factor *= 0.68
        elif player.age >= 30:
            age_factor *= 0.82
        contract_factor = {
            0: 0.58,
            1: 0.76,
            2: 0.90,
        }.get(player.contract_years, 1.0)
        rating_factor = 0.72 + player.rating / 160.0
        listing_factor = 0.94 if player.listed_for_sale else 1.0
        buyer_factor = 1.0
        if buyer_id is not None:
            buyer = self._require_club(buyer_id)
            seller = self.clubs[player.club_id]
            buyer_factor += max(-0.08, min(0.10, (buyer.prestige - seller.prestige) / 500.0))
        return max(1, math.ceil(value * age_factor * contract_factor * rating_factor * listing_factor * buyer_factor))

    def submit_user_bid(
        self,
        *,
        player_id: str,
        fee: int,
        annual_wage: int,
        contract_years: int,
    ) -> Offer:
        """Open a bid for an AI club's player and let its seller answer."""
        window_key = self._require_open_window()
        player = self._require_player(player_id)
        if player.club_id == self.user_club_id:
            raise ValueError("Use the incoming-offer decision for your own player")
        self._validate_terms(fee, annual_wage, contract_years)
        buyer = self.clubs[self.user_club_id]
        if fee > min(buyer.cash, buyer.transfer_budget):
            raise ValueError("The fee exceeds your available transfer budget")
        if annual_wage > max(0, buyer.annual_wage_budget - buyer.payroll):
            raise ValueError("The wage exceeds your available wage budget")
        self._check_roster_move(player, buyer.id, player.club_id)
        offer = self._open_offer(
            player=player,
            buyer_id=buyer.id,
            fee=fee,
            annual_wage=annual_wage,
            contract_years=contract_years,
            window_key=window_key,
        )
        self._ai_seller_response(offer)
        return offer

    def respond_to_incoming_offer(
        self,
        offer_id: str,
        decision: str,
        *,
        counter_fee: int | None = None,
    ) -> Offer:
        """Accept, reject, or counter an AI bid for a user-club player."""
        offer = self._require_offer(offer_id)
        if offer.seller_id != self.user_club_id or offer.status != "seller_response":
            raise ValueError("This offer is not waiting for your club's decision")
        self._require_offer_window(offer)
        action = decision.strip().lower()
        if action == "reject":
            self._reject(offer, "The club rejected the offer")
        elif action == "accept":
            self._fee_agreed(offer)
        elif action == "counter":
            if counter_fee is None or counter_fee <= 0:
                raise ValueError("A positive counter_fee is required")
            if offer.rounds >= MAX_NEGOTIATION_ROUNDS:
                self._reject(offer, "The fee negotiation limit was reached")
            else:
                offer.rounds += 1
                offer.counter_fee = counter_fee
                self._ai_buyer_response(offer, counter_fee)
        else:
            raise ValueError("decision must be accept, reject, or counter")
        return offer

    def respond_to_fee_counter(self, offer_id: str, decision: str, *, fee: int | None = None) -> Offer:
        """Answer an AI seller's fee counteroffer to the user's bid."""
        offer = self._require_offer(offer_id)
        if offer.buyer_id != self.user_club_id or offer.status != "buyer_fee_response":
            raise ValueError("This offer is not waiting for your fee decision")
        self._require_offer_window(offer)
        action = decision.strip().lower()
        if action == "reject":
            self._reject(offer, "You walked away from the fee negotiation")
        elif action == "accept":
            assert offer.counter_fee is not None
            offer.fee = offer.counter_fee
            offer.counter_fee = None
            self._fee_agreed(offer)
        elif action == "counter":
            if fee is None or fee <= 0:
                raise ValueError("A positive fee is required for a counteroffer")
            if offer.rounds >= MAX_NEGOTIATION_ROUNDS:
                self._reject(offer, "The fee negotiation limit was reached")
            elif fee > min(self.clubs[offer.buyer_id].cash, self.clubs[offer.buyer_id].transfer_budget):
                raise ValueError("Your counteroffer exceeds the available transfer budget")
            else:
                offer.fee = fee
                offer.counter_fee = None
                offer.rounds += 1
                self._ai_seller_response(offer)
        else:
            raise ValueError("decision must be accept, reject, or counter")
        return offer

    def respond_to_player_counter(self, offer_id: str, accept: bool) -> Offer:
        """Accept or decline the player's current wage request or final counter."""
        offer = self._require_offer(offer_id)
        if offer.buyer_id != self.user_club_id or offer.status != "buyer_player_response":
            raise ValueError("This offer is not waiting for your wage decision")
        self._require_offer_window(offer)
        if not accept:
            self._reject(offer, "You declined the player's wage request")
            return offer
        assert offer.counter_wage is not None
        offer.annual_wage = offer.counter_wage
        offer.counter_wage = None
        self._commit_transfer(offer)
        return offer

    def counter_player_wage(self, offer_id: str, annual_wage: int) -> Offer:
        """Counter a player's wage request once; the player may make one final counter."""
        offer = self._require_offer(offer_id)
        if offer.buyer_id != self.user_club_id or offer.status != "buyer_player_response":
            raise ValueError("This offer is not waiting for your wage decision")
        self._require_offer_window(offer)
        if annual_wage <= 0:
            raise ValueError("A positive annual wage is required")
        buyer = self.clubs[offer.buyer_id]
        wage_room = max(0, buyer.annual_wage_budget - buyer.payroll)
        if annual_wage > wage_room:
            raise ValueError("Your wage counter exceeds the available wage budget")
        if offer.user_wage_countered:
            self._reject(offer, "The player made a final wage counter and will not negotiate again")
            return offer
        if offer.counter_wage is None or offer.counter_wage <= 0:
            raise ValueError("The player's wage request is missing")

        accept_floor = max(1, offer.counter_wage * 90 // 100)
        counter_floor = max(1, offer.counter_wage * 85 // 100)
        offer.annual_wage = annual_wage
        offer.rounds += 1
        if annual_wage >= accept_floor:
            offer.counter_wage = None
            self._commit_transfer(offer)
        elif annual_wage >= counter_floor:
            offer.counter_wage = accept_floor
            offer.user_wage_countered = True
            offer.message = "The player made a final wage counter"
        else:
            self._reject(offer, "The player rejected the wage counter")
        return offer

    def withdraw_user_bid(self, offer_id: str) -> Offer:
        offer = self._require_offer(offer_id)
        if offer.buyer_id != self.user_club_id or offer.status not in ACTIVE_STATUSES:
            raise ValueError("This bid cannot be withdrawn")
        self._reject(offer, "The user withdrew the bid", status="withdrawn")
        return offer

    def tick_ai_market(self) -> list[Offer]:
        """Run one bounded market tick for every AI club in the open window.

        AI-to-AI negotiations are resolved through the same fee and wage rules.
        An offer on a user-club player remains in the user's inbox for a choice.
        """
        window_key = self._require_open_window()
        activity = self._activity(window_key)
        created: list[Offer] = []
        buyers = [club for club in self.clubs.values() if club.id != self.user_club_id]
        buyers.sort(key=lambda club: (-self._need_score(club.id), club.id))
        for buyer in buyers:
            if buyer.id in activity["incoming"] or buyer.id in activity["attempted_buyers"]:
                continue
            if buyer.transfer_budget <= 0 or buyer.cash <= 0:
                continue
            selected = self._choose_ai_target(buyer.id)
            if selected is None:
                continue
            player, fee, wage = selected
            activity["attempted_buyers"].append(buyer.id)
            offer = self._open_offer(
                player=player,
                buyer_id=buyer.id,
                fee=fee,
                annual_wage=wage,
                contract_years=min(3, max(1, player.contract_years)),
                window_key=window_key,
            )
            created.append(offer)
            if offer.seller_id == self.user_club_id:
                offer.message = "An AI club is waiting for your decision"
            else:
                self._resolve_ai_to_ai(offer)
        return created

    def advance_turn(
        self,
        *,
        match_income: Mapping[str, int] | None = None,
        performance_income: Mapping[str, int] | None = None,
    ) -> None:
        """Settle club finances and move to the next season turn.

        The game integration should pass verified match and competition income.
        Prototype-generated income uses each club's seeded revenue_per_turn.
        """
        old_window = self.current_window_key
        self._settle_finances(match_income or {}, performance_income or {})
        if self.turn >= self.season_turns - 1:
            self._expire_active_offers("The season ended")
            self.season += 1
            self.turn = 0
            for player in self.players.values():
                player.age = min(50, player.age + 1)
                player.contract_years = max(0, player.contract_years - 1)
            self._renew_ai_contracts()
        else:
            self.turn += 1
            if old_window is not None and self.current_window_key != old_window:
                self._expire_window_offers(old_window)
        self._validate()

    def _renew_ai_contracts(self) -> None:
        """Renew up to three affordable core-player contracts for each AI club."""
        for club in self.clubs.values():
            if club.id == self.user_club_id:
                continue
            roster = self.players_for_club(club.id)
            if not roster:
                continue
            club_strength = sum(player.rating for player in roster) // len(roster)
            for _ in range(AI_CONTRACT_RENEWAL_LIMIT):
                wage_room = max(0, club.annual_wage_budget - club.payroll)
                best: Player | None = None
                best_wage = 0
                best_score = -1
                for player in roster:
                    if player.contract_years > 1 or player.rating + 5 < club_strength:
                        continue
                    if player.rating >= club_strength + 10:
                        demand = player.annual_wage * 110 // 100
                    elif player.rating + 10 < club_strength:
                        demand = player.annual_wage * 95 // 100
                    else:
                        demand = player.annual_wage * 105 // 100
                    if player.contract_years == 1:
                        demand = demand * 105 // 100
                    demand = max(1, demand)
                    if demand > player.annual_wage + wage_room:
                        continue
                    score = player.rating * 4 + (2 if player.contract_years == 0 else 1)
                    if score > best_score:
                        best = player
                        best_wage = demand
                        best_score = score
                if best is None:
                    break
                old_wage = best.annual_wage
                best.annual_wage = best_wage
                best.contract_years = AI_CONTRACT_RENEWAL_YEARS
                club.payroll += best_wage - old_wage
                self._append_finance_entry(
                    club.id,
                    "contract_renewal",
                    payroll_delta=best_wage - old_wage,
                    note=f"AI renewed {AI_CONTRACT_RENEWAL_YEARS}-season contract for {best.name}",
                )

    def save(self, path: str | Path) -> None:
        """Write a versioned, checksummed JSON save using atomic replacement."""
        self._validate()
        target = Path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        payload = self._to_payload()
        canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        envelope = {
            "schema_version": SCHEMA_VERSION,
            "sha256": hashlib.sha256(canonical.encode("utf-8")).hexdigest(),
            "payload": payload,
        }
        fd, temporary_name = tempfile.mkstemp(prefix=f".{target.name}.", suffix=".tmp", dir=target.parent)
        try:
            with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
                json.dump(envelope, stream, sort_keys=True, ensure_ascii=False, indent=2)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary_name, target)
        except Exception:
            try:
                os.unlink(temporary_name)
            except OSError:
                pass
            raise

    @classmethod
    def load(cls, path: str | Path) -> "CareerMarket":
        with Path(path).open("r", encoding="utf-8") as stream:
            envelope = json.load(stream)
        envelope_version = envelope.get("schema_version")
        if envelope_version not in {PREVIOUS_SCHEMA_VERSION, SCHEMA_VERSION}:
            raise ValueError("Unsupported career-market save version")
        payload = envelope.get("payload")
        if not isinstance(payload, dict):
            raise ValueError("Career-market save has no payload")
        if payload.get("schema_version") != envelope_version:
            raise ValueError("Career-market save versions do not match")
        canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        digest = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
        if digest != envelope.get("sha256"):
            raise ValueError("Career-market save checksum does not match")
        if envelope_version == PREVIOUS_SCHEMA_VERSION:
            payload["schema_version"] = SCHEMA_VERSION
        state = cls(
            save_slot=payload["save_slot"],
            user_club_id=payload["user_club_id"],
            season=payload["season"],
            turn=payload["turn"],
            season_turns=payload["season_turns"],
            clubs={key: Club(**value) for key, value in payload["clubs"].items()},
            players={key: Player(**value) for key, value in payload["players"].items()},
            offers={key: Offer(**value) for key, value in payload.get("offers", {}).items()},
            history=[TransferRecord(**value) for value in payload.get("history", [])],
            finance_ledger=[FinanceEntry(**value) for value in payload.get("finance_ledger", [])],
            window_activity=payload.get("window_activity", {}),
            schema_version=SCHEMA_VERSION,
        )
        state._validate()
        return state

    def _to_payload(self) -> dict[str, Any]:
        return {
            "schema_version": self.schema_version,
            "save_slot": self.save_slot,
            "user_club_id": self.user_club_id,
            "season": self.season,
            "turn": self.turn,
            "season_turns": self.season_turns,
            "clubs": {key: asdict(value) for key, value in sorted(self.clubs.items())},
            "players": {key: asdict(value) for key, value in sorted(self.players.items())},
            "offers": {key: asdict(value) for key, value in sorted(self.offers.items())},
            "history": [asdict(record) for record in self.history],
            "finance_ledger": [asdict(entry) for entry in self.finance_ledger],
            "window_activity": self.window_activity,
        }

    def _seed_missing_economies(self) -> None:
        for club in self.clubs.values():
            roster = self.players_for_club(club.id)
            real_payroll = sum(player.annual_wage for player in roster)
            squad_value = sum(max(1, player.market_value) for player in roster)
            if club.payroll == 0:
                club.payroll = real_payroll
            if club.cash == 0 and club.transfer_budget == 0 and club.annual_wage_budget == 0:
                club.cash = max(1_000, squad_value // 3, real_payroll * 2)
                club.transfer_budget = min(
                    club.cash,
                    max(250, squad_value // 12, real_payroll // 2),
                )
                club.annual_wage_budget = max(1, math.ceil(real_payroll * 1.15))
                club.revenue_per_turn = max(
                    club.revenue_per_turn,
                    1,
                    real_payroll // self.season_turns,
                    squad_value // (self.season_turns * 50),
                )
            else:
                if club.annual_wage_budget == 0:
                    club.annual_wage_budget = max(1, math.ceil(real_payroll * 1.15))
                if club.revenue_per_turn == 0:
                    club.revenue_per_turn = max(1, real_payroll // self.season_turns)

    def _validate(self) -> None:
        if self.schema_version != SCHEMA_VERSION:
            raise ValueError("Career-market schema version is invalid")
        if not self.save_slot or self.user_club_id not in self.clubs:
            raise ValueError("Career-market save identity is invalid")
        if self.season < 1 or self.season_turns < 4 or not 0 <= self.turn < self.season_turns:
            raise ValueError("Career-market season cursor is invalid")
        if not self.players or not self.clubs:
            raise ValueError("Career-market state needs players and clubs")
        for key, club in self.clubs.items():
            if key != club.id:
                raise ValueError(f"Club dictionary key mismatch: {key}")
            if club.transfer_budget > club.cash:
                raise ValueError(f"Transfer budget exceeds cash for {club.id}")
            actual_payroll = sum(
                player.annual_wage for player in self.players.values() if player.club_id == club.id
            )
            if actual_payroll != club.payroll:
                raise ValueError(f"Payroll does not match roster for {club.id}")
        for key, player in self.players.items():
            if key != player.id or player.club_id not in self.clubs:
                raise ValueError(f"Player ownership is invalid: {key}")
        for key, offer in self.offers.items():
            if key != offer.id or offer.player_id not in self.players:
                raise ValueError(f"Offer identity is invalid: {key}")
            if offer.buyer_id not in self.clubs or offer.seller_id not in self.clubs:
                raise ValueError(f"Offer clubs are invalid: {key}")
            if offer.status not in ACTIVE_STATUSES | TERMINAL_STATUSES:
                raise ValueError(f"Offer status is invalid: {key}")
        for entry in self.finance_ledger:
            if entry.club_id not in self.clubs:
                raise ValueError(f"Finance entry references an unknown club: {entry.id}")

    def _require_open_window(self) -> str:
        key = self.current_window_key
        if key is None:
            raise ValueError("The transfer window is closed")
        return key

    def _require_offer_window(self, offer: Offer) -> None:
        if offer.window_key != self.current_window_key:
            self._reject(offer, "The transfer window closed")
            raise ValueError("The transfer window closed before the offer was resolved")

    def _require_club(self, club_id: str) -> Club:
        try:
            return self.clubs[club_id]
        except KeyError as error:
            raise ValueError(f"Unknown club: {club_id}") from error

    def _require_player(self, player_id: str) -> Player:
        try:
            return self.players[player_id]
        except KeyError as error:
            raise ValueError(f"Unknown player: {player_id}") from error

    def _require_offer(self, offer_id: str) -> Offer:
        try:
            return self.offers[offer_id]
        except KeyError as error:
            raise ValueError(f"Unknown offer: {offer_id}") from error

    @staticmethod
    def _validate_terms(fee: int, annual_wage: int, contract_years: int) -> None:
        if fee <= 0 or annual_wage < 0:
            raise ValueError("Fee must be positive and wage cannot be negative")
        if not 1 <= contract_years <= MAX_CONTRACT_YEARS:
            raise ValueError(f"Contract length must be 1..{MAX_CONTRACT_YEARS} years")

    def _activity(self, window_key: str) -> dict[str, list[str]]:
        activity = self.window_activity.setdefault(
            window_key,
            {"incoming": [], "outgoing": [], "completed_outgoing": [], "attempted_buyers": []},
        )
        activity.setdefault("completed_outgoing", [])
        return activity

    def _open_offer(
        self,
        *,
        player: Player,
        buyer_id: str,
        fee: int,
        annual_wage: int,
        contract_years: int,
        window_key: str,
    ) -> Offer:
        seller_id = player.club_id
        self._check_roster_move(player, buyer_id, seller_id)
        if self._has_active_player_offer(player.id):
            raise ValueError(f"Player {player.id} already has a live negotiation")
        activity = self._activity(window_key)
        if buyer_id in activity["incoming"]:
            raise ValueError(f"{self.clubs[buyer_id].name} already has a purchase in this window")
        if seller_id in activity["outgoing"] and seller_id != self.user_club_id:
            raise ValueError(f"{self.clubs[seller_id].name} already has a sale in this window")
        activity["incoming"].append(buyer_id)
        if seller_id != self.user_club_id:
            activity["outgoing"].append(seller_id)
        offer_id = f"{self.save_slot}-s{self.season}-t{self.turn}-o{len(self.offers) + 1}"
        offer = Offer(
            id=offer_id,
            player_id=player.id,
            buyer_id=buyer_id,
            seller_id=seller_id,
            fee=fee,
            annual_wage=annual_wage,
            contract_years=contract_years,
            status="seller_response",
            season=self.season,
            turn=self.turn,
            window_key=window_key,
        )
        self.offers[offer.id] = offer
        return offer

    def _check_roster_move(self, player: Player, buyer_id: str, seller_id: str) -> None:
        buyer = self._require_club(buyer_id)
        seller = self._require_club(seller_id)
        if buyer_id == seller_id:
            raise ValueError("A player cannot transfer to the same club")
        buyer_size = len(self.players_for_club(buyer_id))
        seller_roster = self.players_for_club(seller_id)
        if buyer_size >= buyer.max_squad_size:
            raise ValueError(f"{buyer.name} is at its maximum squad size")
        if len(seller_roster) - 1 < seller.min_squad_size:
            raise ValueError(f"{seller.name} cannot sell below its minimum squad size")
        group = position_group(player.position)
        group_count = sum(position_group(item.position) == group for item in seller_roster)
        floor = SELLER_POSITION_FLOORS.get(group, 1)
        if group_count - 1 < floor:
            raise ValueError(f"{seller.name} needs to keep at least {floor} {group} player(s)")

    def _has_active_player_offer(self, player_id: str) -> bool:
        return any(
            offer.player_id == player_id and offer.status in ACTIVE_STATUSES
            for offer in self.offers.values()
        )

    def _seller_floor(self, offer: Offer) -> int:
        player = self.players[offer.player_id]
        value = self.player_value(player.id, offer.buyer_id)
        floor_factor = 0.80 if player.listed_for_sale else 0.94
        if player.contract_years <= 1:
            floor_factor *= 0.82
        return max(1, math.ceil(value * floor_factor))

    def _fee_agreed(self, offer: Offer) -> None:
        if offer.status in TERMINAL_STATUSES:
            return
        buyer = self.clubs[offer.buyer_id]
        if offer.fee > min(buyer.cash, buyer.transfer_budget):
            self._reject(offer, "The buyer no longer has the agreed fee available")
            return
        self._begin_player_negotiation(offer)

    def _begin_player_negotiation(self, offer: Offer) -> None:
        buyer = self.clubs[offer.buyer_id]
        player = self.players[offer.player_id]
        wage_room = max(0, buyer.annual_wage_budget - buyer.payroll)
        if offer.annual_wage > wage_room:
            self._reject(offer, "The proposed wage exceeds the buyer's wage budget")
            return
        expected = self._player_wage_demand(player, buyer)
        if offer.annual_wage >= expected:
            self._commit_transfer(offer)
            return
        if not offer.player_countered and offer.annual_wage >= math.ceil(expected * 0.78):
            if expected > wage_room:
                self._reject(offer, "The player wage request exceeds the buyer's wage budget")
                return
            offer.player_countered = True
            offer.counter_wage = expected
            if offer.buyer_id == self.user_club_id:
                offer.status = "buyer_player_response"
                offer.message = "The player countered on wages"
            else:
                offer.annual_wage = expected
                offer.counter_wage = None
                self._commit_transfer(offer)
            return
        self._reject(offer, "The player rejected the wage offer")

    def _player_wage_demand(self, player: Player, buyer: Club) -> int:
        seller = self.clubs[player.club_id]
        prestige_change = max(-0.10, min(0.30, (buyer.prestige - seller.prestige) / 250.0))
        star_premium = max(0, player.rating - 82) * 0.004
        multiplier = 1.06 + prestige_change + star_premium
        return max(1, player.annual_wage, math.ceil(max(1, player.annual_wage) * multiplier))

    def _commit_transfer(self, offer: Offer) -> None:
        player = self.players[offer.player_id]
        buyer = self.clubs[offer.buyer_id]
        seller = self.clubs[offer.seller_id]
        if offer.window_key != self.current_window_key:
            self._reject(offer, "The transfer window closed before the deal was completed")
            return
        if player.club_id != seller.id:
            self._reject(offer, "The player is no longer at the selling club")
            return
        if offer.fee > min(buyer.cash, buyer.transfer_budget):
            self._reject(offer, "The buyer cannot afford the agreed fee")
            return
        old_wage = player.annual_wage
        wage_room = buyer.annual_wage_budget - buyer.payroll
        if offer.annual_wage > wage_room:
            self._reject(offer, "The buyer cannot afford the agreed player wage")
            return
        try:
            self._check_roster_move(player, buyer.id, seller.id)
        except ValueError as error:
            self._reject(offer, str(error))
            return

        # All failure-prone checks happen before this point; then update the
        # ownership and both clubs' ledgers as one in-memory transaction.
        buyer.cash -= offer.fee
        buyer.transfer_budget -= offer.fee
        buyer_budget_delta = -offer.fee
        seller.cash += offer.fee
        seller_budget_before = seller.transfer_budget
        seller.transfer_budget = min(
            seller.cash,
            seller.transfer_budget + math.floor(offer.fee * 0.65),
        )
        seller_budget_delta = seller.transfer_budget - seller_budget_before
        buyer.payroll += offer.annual_wage
        seller.payroll -= old_wage
        player.club_id = buyer.id
        player.annual_wage = offer.annual_wage
        player.contract_years = offer.contract_years
        player.listed_for_sale = False
        offer.status = "completed"
        offer.message = "Transfer completed"
        activity = self._activity(offer.window_key)
        if seller.id == self.user_club_id:
            if seller.id not in activity["completed_outgoing"]:
                activity["completed_outgoing"].append(seller.id)
            for pending in self.offers.values():
                if (
                    pending.id != offer.id
                    and pending.window_key == offer.window_key
                    and pending.seller_id == seller.id
                    and pending.status in ACTIVE_STATUSES
                ):
                    self._reject(pending, "The club completed another sale this window")
        self.history.append(
            TransferRecord(
                offer_id=offer.id,
                player_id=player.id,
                player_name=player.name,
                buyer_id=buyer.id,
                seller_id=seller.id,
                fee=offer.fee,
                annual_wage=offer.annual_wage,
                contract_years=offer.contract_years,
                season=self.season,
                turn=self.turn,
                negotiation_rounds=offer.rounds,
            )
        )
        self._append_finance_entry(
            buyer.id,
            "transfer_fee_paid",
            cash_delta=-offer.fee,
            transfer_budget_delta=buyer_budget_delta,
            payroll_delta=offer.annual_wage,
            note=f"Signed {player.name}",
        )
        self._append_finance_entry(
            seller.id,
            "transfer_fee_received",
            cash_delta=offer.fee,
            transfer_budget_delta=seller_budget_delta,
            payroll_delta=-old_wage,
            note=f"Sold {player.name}",
        )
        self._validate()

    def _ai_seller_response(self, offer: Offer) -> None:
        if offer.seller_id == self.user_club_id:
            return
        if offer.status in TERMINAL_STATUSES:
            return
        floor = self._seller_floor(offer)
        if offer.fee >= floor:
            self._fee_agreed(offer)
        elif offer.rounds >= MAX_NEGOTIATION_ROUNDS or offer.fee < math.ceil(floor * 0.65):
            self._reject(offer, "The selling club rejected the fee")
        else:
            offer.counter_fee = floor
            offer.status = "buyer_fee_response"
            offer.rounds += 1
            offer.message = "The selling club countered on the fee"

    def _ai_buyer_response(self, offer: Offer, seller_counter: int) -> None:
        buyer = self.clubs[offer.buyer_id]
        player = self.players[offer.player_id]
        max_fee = min(
            buyer.cash,
            buyer.transfer_budget,
            math.ceil(self.player_value(player.id, buyer.id) * 1.35),
        )
        if seller_counter <= max_fee:
            offer.fee = seller_counter
            offer.counter_fee = None
            self._fee_agreed(offer)
        elif offer.rounds >= MAX_NEGOTIATION_ROUNDS or max_fee <= offer.fee:
            self._reject(offer, "The buying club walked away from the fee")
        else:
            buyer_counter = min(max_fee, max(offer.fee + 1, (offer.fee + seller_counter) // 2))
            offer.fee = buyer_counter
            offer.counter_fee = None
            offer.status = "seller_response"
            offer.rounds += 1
            offer.message = "The buying club countered the fee"

    def _resolve_ai_to_ai(self, offer: Offer) -> None:
        buyer = self.clubs[offer.buyer_id]
        player = self.players[offer.player_id]
        floor = self._seller_floor(offer)
        buyer_max = min(
            buyer.cash,
            buyer.transfer_budget,
            math.ceil(self.player_value(player.id, buyer.id) * 1.35),
        )
        if offer.fee >= floor:
            self._fee_agreed(offer)
            return
        if offer.fee < math.ceil(floor * 0.65) or buyer_max <= offer.fee:
            self._reject(offer, "AI clubs could not agree on a transfer fee")
            return
        offer.rounds += 1
        if floor <= buyer_max:
            offer.fee = floor
        else:
            offer.fee = min(buyer_max, max(offer.fee + 1, (offer.fee + floor) // 2))
        offer.rounds += 1
        if offer.fee < floor:
            self._reject(offer, "AI clubs could not agree on a transfer fee")
            return
        self._fee_agreed(offer)

    def _commit_ai_player_counter(self, offer: Offer) -> None:
        if offer.buyer_id == self.user_club_id:
            offer.status = "buyer_player_response"
            offer.message = "The player countered on wages"
            return
        assert offer.counter_wage is not None
        buyer = self.clubs[offer.buyer_id]
        wage_room = max(0, buyer.annual_wage_budget - buyer.payroll)
        if offer.counter_wage > wage_room:
            self._reject(offer, "The AI buyer could not meet the player's wage request")
            return
        offer.annual_wage = offer.counter_wage
        offer.counter_wage = None
        self._commit_transfer(offer)

    def _choose_ai_target(self, buyer_id: str) -> tuple[Player, int, int] | None:
        buyer = self.clubs[buyer_id]
        wage_room = max(0, buyer.annual_wage_budget - buyer.payroll)
        if wage_room <= 0:
            return None
        wanted = self._most_needed_position(buyer_id)
        buyer_roster = self.players_for_club(buyer_id)
        average_rating = sum(player.rating for player in buyer_roster) / max(1, len(buyer_roster))
        activity = self._activity(self._require_open_window())
        candidates: list[tuple[float, Player, int, int]] = []
        for player in self.players.values():
            if player.club_id == buyer_id or self._has_active_player_offer(player.id):
                continue
            seller = self.clubs[player.club_id]
            if seller.id != self.user_club_id and seller.id in activity["outgoing"]:
                continue
            if seller.id == self.user_club_id and seller.id in activity["completed_outgoing"]:
                continue
            if seller.id == self.user_club_id and not player.listed_for_sale:
                continue
            try:
                self._check_roster_move(player, buyer_id, seller.id)
            except ValueError:
                continue
            value = self.player_value(player.id, buyer_id)
            max_fee = min(buyer.cash, buyer.transfer_budget, math.ceil(value * 1.35))
            fee = max(1, math.floor(value * 0.90))
            demand = self._player_wage_demand(player, buyer)
            if fee > max_fee or demand > wage_room:
                continue
            group = position_group(player.position)
            group_fit = 1 if group == wanted else 0
            seller_surplus = self._position_count(seller.id, group) - SELLER_POSITION_FLOORS.get(group, 1)
            score = (
                group_fit * 10_000
                + seller_surplus * 300
                + (player.rating - average_rating) * 12
                + (250 if player.listed_for_sale else 0)
                - value / max(1, buyer.transfer_budget) * 100
                - max(0, player.age - 29) * 20
            )
            candidates.append((score, player, fee, demand))
        if not candidates:
            return None
        candidates.sort(key=lambda item: (-item[0], item[1].id))
        _, player, fee, wage = candidates[0]
        return player, fee, wage

    def _most_needed_position(self, club_id: str) -> str:
        counts = {group: self._position_count(club_id, group) for group in BUYER_POSITION_TARGETS}
        roster_size = len(self.players_for_club(club_id))
        scale = min(1.0, roster_size / sum(BUYER_POSITION_TARGETS.values()))
        deficits = {
            group: max(0.0, target * scale - counts[group])
            for group, target in BUYER_POSITION_TARGETS.items()
        }
        if max(deficits.values(), default=0) > 0:
            return max(deficits, key=lambda group: (deficits[group], group))
        ratings = {
            group: [player.rating for player in self.players_for_club(club_id) if position_group(player.position) == group]
            for group in BUYER_POSITION_TARGETS
        }
        return min(
            BUYER_POSITION_TARGETS,
            key=lambda group: (sum(ratings[group]) / max(1, len(ratings[group])), group),
        )

    def _need_score(self, club_id: str) -> float:
        group = self._most_needed_position(club_id)
        scale = min(1.0, len(self.players_for_club(club_id)) / sum(BUYER_POSITION_TARGETS.values()))
        return max(0.0, BUYER_POSITION_TARGETS[group] * scale - self._position_count(club_id, group))

    def _position_count(self, club_id: str, group: str) -> int:
        return sum(
            position_group(player.position) == group
            for player in self.players_for_club(club_id)
        )

    def _settle_finances(
        self,
        match_income: Mapping[str, int],
        performance_income: Mapping[str, int],
    ) -> None:
        unknown_ids = (set(match_income) | set(performance_income)) - set(self.clubs)
        if unknown_ids:
            raise ValueError(f"Income references unknown club(s): {', '.join(sorted(unknown_ids))}")
        for club in self.clubs.values():
            base_income = club.revenue_per_turn
            match_receipts = max(0, int(match_income.get(club.id, 0)))
            performance_receipts = max(0, int(performance_income.get(club.id, 0)))
            wage_bill = math.ceil(club.payroll / self.season_turns)
            self._append_finance_entry(club.id, "club_income", cash_delta=base_income)
            if match_receipts:
                self._append_finance_entry(club.id, "match_income", cash_delta=match_receipts)
            if performance_receipts:
                self._append_finance_entry(
                    club.id,
                    "performance_income",
                    cash_delta=performance_receipts,
                )
            available_cash = club.cash + base_income + match_receipts + performance_receipts
            wage_paid = min(available_cash, wage_bill)
            club.cash = available_cash - wage_paid
            if wage_paid:
                self._append_finance_entry(club.id, "wages_paid", cash_delta=-wage_paid)
            operating_surplus = max(0, base_income + match_receipts + performance_receipts - wage_bill)
            budget_before = club.transfer_budget
            club.transfer_budget = min(
                club.cash,
                club.transfer_budget + math.floor(operating_surplus * 0.35),
            )
            if club.transfer_budget != budget_before:
                self._append_finance_entry(
                    club.id,
                    "budget_reinvestment",
                    transfer_budget_delta=club.transfer_budget - budget_before,
                    note="35% of positive turn surplus",
                )

    def _append_finance_entry(
        self,
        club_id: str,
        category: str,
        *,
        cash_delta: int = 0,
        transfer_budget_delta: int = 0,
        payroll_delta: int = 0,
        note: str = "",
    ) -> None:
        self.finance_ledger.append(
            FinanceEntry(
                id=f"s{self.season}-t{self.turn}-f{len(self.finance_ledger) + 1}",
                club_id=club_id,
                season=self.season,
                turn=self.turn,
                category=category,
                cash_delta=cash_delta,
                transfer_budget_delta=transfer_budget_delta,
                payroll_delta=payroll_delta,
                note=note,
            )
        )

    def _expire_window_offers(self, window_key: str) -> None:
        for offer in self.offers.values():
            if offer.window_key == window_key and offer.status in ACTIVE_STATUSES:
                self._reject(offer, "The transfer window closed", status="expired")

    def _expire_active_offers(self, message: str) -> None:
        for offer in self.offers.values():
            if offer.status in ACTIVE_STATUSES:
                self._reject(offer, message, status="expired")

    @staticmethod
    def _reject(offer: Offer, message: str, *, status: str = "rejected") -> None:
        offer.status = status
        offer.counter_fee = None
        offer.counter_wage = None
        offer.message = message


__all__ = [
    "CareerMarket",
    "Club",
    "FinanceEntry",
    "Offer",
    "Player",
    "TransferRecord",
    "position_group",
]

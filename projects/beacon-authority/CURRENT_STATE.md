# Current state — beacon authority

| Capability | Status | Notes |
|------------|--------|-------|
| Design, part 1 (the real beacon) | **Decided** | [DESIGN.md](DESIGN.md), decisions B001–B009; next is ba1 |
| Design, part 2 (promotion) | **Outlined** | Three cases agreed; detailed design after part 1 |
| Beacon certificate / signed tip / generations | **No** | Phases ba1–ba7 |
| Equivocation halt | **No** | Phase ba6 |
| Recovery set | **No** | Phase ba1 / ba7 |

Today the beacon is authenticated only by the Amp peer id pinned in its downstream's config (one
hop); see DESIGN § Where we are today.

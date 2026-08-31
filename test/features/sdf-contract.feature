Feature: SHIELD SDF producer contract ()
  docs/shield/sdf-contract.md must enumerate the two SDF producer tiers and the
  malformed-SDF regeneration rule so every producer and the meshing promotion lane
  agree on the data shape from `sdf_data` size alone (). The SdfContractDocLint
  ctest checks the contract doc against the strings quoted in the Then/And steps below —
  those quoted needles are the single source of truth for what the doc must state (see
  tools/check_sdf_contract_doc.py).

  Scenario: The contract enumerates both producer tiers and the malformed rule
    Given the SDF contract doc docs/shield/sdf-contract.md
    Then the contract documents "Two Producer Tiers"
    And the contract documents "full unit-step lattice"
    And the contract documents "kFullSdfLattice"
    And the contract documents "coarse heightmap-only"
    And the contract documents "GenerateCoarseHeightfieldTerrain"
    And the contract documents "malformed"
    And the contract documents "regenerates"

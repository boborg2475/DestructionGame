// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A DIAGNOSTIC MORTAR-SENSITIVITY PROBE, NOT A PRODUCTION CHANGE AND NOT A PERMANENT RED (owner-approved
 * item 6c). It MEASURES how much the realistic shed's verdicts depend on the OPTIMISTIC end of the mortar
 * literature the shipped `GeneralPurposeMortar` bed row sits at — cohesion 0.9 / tensile 0.7 MPa, the top of
 * the van der Pluijm / Lourenço / EN 1052-3 body, whose MEDIAN for GP mortar on clay is nearer 0.3-0.6 shear
 * and 0.3-0.5 flexural. It does NOT touch ConnectionProfiles.cpp: the shipped strengths stay exactly as they
 * are. Instead it re-authors the BED joints' strength ON THE BUILT STRUCTURE, in-test, to each pair in a
 * ladder from the shipped 0.9/0.7 down to a median 0.3/0.25, and re-runs the shed's key scenarios through the
 * production router (SolveAndBreak) at each strength, logging one SHEDMORTARSENS line per (strength, scenario)
 * so the whole sensitivity curve is legible in Saved/Logs/DestructionGame.log.
 *
 * WHAT MOVES AND WHAT STAYS. Only the BED joints move — the horizontal brick-brick mortar joints that carry
 * the wall down, identified independently of the shipped strength by the SAME classification the builder uses
 * (both faces ClayBrick, interface normal vertical). The weak PERPEND row (0.2/0.1, the vertical head/corner
 * joints from item 6b) and the timber DryStone bearings are left exactly as built. So this isolates the BED
 * mortar's contribution and nothing else.
 *
 * WHY THE RE-AUTHORING GOVERNS. The router decides against EffectiveJointStrength, which pairs the connection
 * with its two faces' materials weakest-link per axis. Both bed faces are ClayBrick (cohesion 3.0, tensile
 * 2.0, BondFactor 1.0), far above the mortar band, so the mortar governs every bed axis and lowering the bed
 * connection genuinely lowers the effective bed strength — verified against MaterialProfiles.cpp before this
 * was written, not assumed.
 *
 * THE THREE SCENARIOS (each re-derived here, not imported):
 *   - STANDING: the as-built shell/gables/roof/porch, no cut — does it still stand? strands anything?
 *   - COLLAPSE: the committed 9-brick back-eaves (course-15) cut that drops the whole free-standing back
 *     gable + its purlins — does the released count change with bed strength?
 *   - DEEPBAND: the full-width low back-wall band (courses 6 and 7, the arch/undermine state) removed — this
 *     already comes down at the shipped strength via the weak perpends; does the bed strength move it?
 *
 * WHAT IT ASSERTS, AND WHAT IT DELIBERATELY DOES NOT. It asserts ONLY the invariants that must hold at every
 * strength: Stranded == 0 for every (strength, scenario) — an honest collapse strands nothing, a routing knot
 * would be a bug wearing the collapse's clothes (DESIGN.md §4) — and, as a CONTROL, that the standing shell
 * stands (0 lost) at the SHIPPED 0.9/0.7. It does NOT assert any specific flip: the VALUE is the logged curve,
 * and the owner decides from it whether the shed's standing is robust or rests on the optimistic number. If a
 * scenario flips (e.g. the standing shed falls at 0.45/0.35) that is the FINDING to report, not a red to fix.
 *
 * The name deliberately omits "DestructionGame" so the full suite never runs it; invoke it with
 * `Automation RunTests ShedRealisticLoads` and grep the log for SHEDMORTARSENS.
 *
 * NEEDS A TICKING WORLD: NO. Boxes, doubles and the router; gravity on. No UWorld, no Chaos, no tick.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace ShedRealisticMortarSensitivityProbeSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* Back-wall geometry, the same literals the collapse and undermine probes key on. */
	const double BackWallYCm = 128.875;
	const double CoursePitchZCm = 7.5;
	const double CourseBaseZCm = 3.25;

	double CourseCentreZ(int32 Course)
	{
		return Course * CoursePitchZCm + CourseBaseZCm;
	}

	bool IsStanding(EPieceSupport Support)
	{
		return Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported;
	}

	int32 StrandedCount(const FStructure& S)
	{
		int32 N = 0;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (!S.IsPieceRemoved(P) && S.GetPieceSupport(P) == EPieceSupport::Stranded)
			{
				++N;
			}
		}
		return N;
	}

	/* A live survivor that lost the earth — present, not one we removed, yet neither Grounded nor Supported. */
	int32 LostEarthCount(const FStructure& S)
	{
		int32 N = 0;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (S.IsPieceRemoved(P))
			{
				continue;
			}
			if (!IsStanding(S.GetPieceSupport(P)))
			{
				++N;
			}
		}
		return N;
	}

	/*
	 * RE-AUTHOR THE BED JOINTS to a (cohesion, tensile) pair, leaving perpends and timber bearings untouched.
	 * A BED is a brick-brick joint whose interface normal is vertical — exactly the builder's own bed/perpend
	 * test (DestructionShed3D::BuildRealistic), re-derived here so the classification does not depend on the
	 * shipped strength value. Keeps the bed row's bearing (compressive 10.0), friction (0.75) and shear ceiling
	 * (2.0) — those are properties of the joint geometry and the unit, not of the bond campaign being swept —
	 * and moves ONLY the two bond axes. Returns how many beds were re-authored.
	 */
	int32 ReauthorBeds(FStructure& S, double CohesionMPa, double TensileMPa)
	{
		int32 Count = 0;
		for (int32 J = 0; J < S.NumConnections(); ++J)
		{
			const FConnection& C = S.GetConnection(J);
			const DestructionProfiles::FMaterialProfile* MatA = S.GetPiece(C.PieceA).Material;
			const DestructionProfiles::FMaterialProfile* MatB = S.GetPiece(C.PieceB).Material;
			const bool bBrickBrick = MatA == &ClayBrick && MatB == &ClayBrick;
			const bool bBed = FMath::Abs(C.InterfaceNormal.GetSafeNormal().Z) > 0.5;
			if (bBrickBrick && bBed)
			{
				FConnectionStrength Rebased{
					/*Compressive*/ 10.0,
					/*ShearCohesion*/ CohesionMPa,
					/*Tensile*/ TensileMPa,
					/*FrictionCoefficient*/ 0.75,
					/*MaxShear*/ 2.0
				};
				S.GetConnectionMutable(J).Strength = Rebased;
				++Count;
			}
		}
		return Count;
	}

	/* The nine back-eaves (course 15) bricks — the committed collapse cut. */
	void CollectBackEaves(const FBrickLayout& L, TArray<int32>& Out)
	{
		const FStructure& S = L.Structure;
		const double ZCm = CourseCentreZ(15);
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (S.IsPieceRemoved(P) || S.GetPiece(P).Material != &ClayBrick || !L.Boxes.IsValidIndex(P))
			{
				continue;
			}
			const FVector Ctr = L.Boxes[P].CentreCm;
			if (FMath::Abs(Ctr.Y - BackWallYCm) < 1.0 && FMath::Abs(Ctr.Z - ZCm) < 1.0)
			{
				Out.Add(P);
			}
		}
	}

	/* The full-width low back-wall band (courses 6 and 7) — the deep-undermine/arch state. */
	void CollectLowBand(const FBrickLayout& L, TArray<int32>& Out)
	{
		const FStructure& S = L.Structure;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (S.IsPieceRemoved(P) || S.GetPiece(P).Material != &ClayBrick || !L.Boxes.IsValidIndex(P))
			{
				continue;
			}
			const FVector Ctr = L.Boxes[P].CentreCm;
			const bool bBackWall = FMath::Abs(Ctr.Y - BackWallYCm) < 1.0;
			const bool bCourse6 = FMath::Abs(Ctr.Z - CourseCentreZ(6)) < 1.0;
			const bool bCourse7 = FMath::Abs(Ctr.Z - CourseCentreZ(7)) < 1.0;
			if (bBackWall && (bCourse6 || bCourse7))
			{
				Out.Add(P);
			}
		}
	}

	enum class EScenario : uint8 { Standing, Collapse, DeepBand };

	const TCHAR* ScenarioName(EScenario Scen)
	{
		switch (Scen)
		{
			case EScenario::Standing: return TEXT("STANDING");
			case EScenario::Collapse: return TEXT("COLLAPSE");
			default:                  return TEXT("DEEPBAND");
		}
	}

	/* The result of one (strength, scenario) run. */
	struct FRun
	{
		int32 Beds = 0;
		int32 Removed = 0;
		int32 Passes = 0;
		int32 LostEarth = 0;
		int32 Stranded = 0;
		bool bStands = true;
	};

	/* Build fresh, re-author beds to (cohesion, tensile), apply the scenario's cut, settle, tally. */
	FRun Measure(EScenario Scen, double CohesionMPa, double TensileMPa, bool& bOutBuilt)
	{
		FRun R;
		FBrickLayout L;
		bOutBuilt = DestructionShed3D::BuildRealistic(L);
		if (!bOutBuilt)
		{
			return R;
		}

		FStructure& S = L.Structure;
		R.Beds = ReauthorBeds(S, CohesionMPa, TensileMPa);

		TArray<int32> Cut;
		if (Scen == EScenario::Collapse)
		{
			CollectBackEaves(L, Cut);
		}
		else if (Scen == EScenario::DeepBand)
		{
			CollectLowBand(L, Cut);
		}
		for (const int32 P : Cut)
		{
			if (S.RemovePiece(P))
			{
				++R.Removed;
			}
		}

		R.Passes = S.SolveAndBreak();
		R.LostEarth = LostEarthCount(S);
		R.Stranded = StrandedCount(S);
		R.bStands = (R.LostEarth == 0);
		return R;
	}
}

/**
 * SWEEP THE BED MORTAR FROM THE SHIPPED OPTIMISTIC 0.9/0.7 DOWN TO A MEDIAN 0.3/0.25 AND RECORD THE SHED'S
 * VERDICT AT EACH STRENGTH, ACROSS THE THREE KEY SCENARIOS. Diagnostic — asserts only the invariants (nothing
 * stranded at any strength; the standing shell stands at the shipped strength as a control). The curve is the
 * output; read it out of the log under SHEDMORTARSENS.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShedRealisticMortarSensitivityProbe,
	"ShedRealisticLoads.Probe.MortarSensitivity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FShedRealisticMortarSensitivityProbe::RunTest(const FString& Parameters)
{
	using namespace ShedRealisticMortarSensitivityProbeSupport;

	/*
	 * THE BED-MORTAR LADDER. The shipped 0.9/0.7 is the OPTIMISTIC top of the literature; 0.45/0.35 is a
	 * MEDIAN-European re-base; the two intermediate rungs and the 0.3/0.25 floor trace the sensitivity between
	 * them. Cohesion (shear bond) and tensile (flexural bond) move together as they do in the real campaigns.
	 */
	struct FStrength { double Cohesion; double Tensile; const TCHAR* Note; };
	const FStrength Ladder[] = {
		{ 0.9,  0.7,  TEXT("shipped / optimistic top") },
		{ 0.6,  0.5,  TEXT("upper-median") },
		{ 0.45, 0.35, TEXT("median European") },
		{ 0.3,  0.25, TEXT("median floor") },
	};

	const EScenario Scenarios[] = { EScenario::Standing, EScenario::Collapse, EScenario::DeepBand };

	for (const FStrength& St : Ladder)
	{
		for (const EScenario Scen : Scenarios)
		{
			bool bBuilt = false;
			const FRun R = Measure(Scen, St.Cohesion, St.Tensile, bBuilt);

			if (!bBuilt)
			{
				AddError(TEXT("BuildRealistic returned false"));
				return false;
			}

			/* THE SENSITIVITY LINE — cohesion, tensile, scenario, then the router's readings. */
			UE_LOG(LogTemp, Display,
				TEXT("SHEDMORTARSENS,%.4g,%.4g,%s,passes=%d,removed=%d,beds=%d,lostearth=%d,stranded=%d,stands=%d,note=<%s>"),
				St.Cohesion, St.Tensile, ScenarioName(Scen),
				R.Passes, R.Removed, R.Beds, R.LostEarth, R.Stranded, R.bStands ? 1 : 0, St.Note);

			/*
			 * INVARIANT — NOTHING IS STRANDED, at any strength, in any scenario. An honest collapse takes the
			 * whole load path with it and strands nobody; a stranded survivor would mean the router declined
			 * rather than the shed genuinely losing support (DESIGN.md §4). This must hold whatever the bed
			 * mortar is, so it is asserted for every cell of the sweep.
			 */
			TestEqual(
				*FString::Printf(
					TEXT("nothing may be Stranded at bed %.4g/%.4g, scenario %s (got %d)"),
					St.Cohesion, St.Tensile, ScenarioName(Scen), R.Stranded),
				R.Stranded, 0);

			/*
			 * CONTROL — THE STANDING SHELL STANDS AT THE SHIPPED STRENGTH. This is the one anchored fact: with
			 * the profile as shipped (0.9/0.7) and no cut, the as-built shed keeps the earth. It pins the probe
			 * to a known-good baseline so a lower rung reading "falls" is a real finding and not a broken
			 * fixture. No lower rung and no other scenario is asserted to stand or fall — that is the curve the
			 * owner reads.
			 */
			if (Scen == EScenario::Standing && St.Cohesion == 0.9 && St.Tensile == 0.7)
			{
				TestTrue(TEXT("CONTROL: the as-built shell stands at the SHIPPED bed mortar 0.9/0.7 (0 lost)"),
					R.bStands);
				TestTrue(TEXT("CONTROL: the shipped standing shell re-authored exactly one bed row family "
					"(some beds present)"), R.Beds > 0);
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

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
 * Diagnostic mortar-sensitivity probe (item 6c); changes no production data. The shipped
 * GeneralPurposeMortar bed row (cohesion 0.9 / tensile 0.7 MPa) is the optimistic top of the
 * van der Pluijm / Lourenço / EN 1052-3 range, whose median is nearer 0.3-0.6 / 0.3-0.5. This
 * re-authors only the brick-brick bed joints on the built shed, down a ladder to 0.3/0.25, and
 * runs three scenarios through SolveAndBreak at each rung, logging one SHEDMORTARSENS line per
 * (strength, scenario). Perpends and timber bearings are untouched. ClayBrick faces are far
 * stronger than the mortar, so the mortar governs the effective bed strength.
 *
 * Scenarios: STANDING (no cut), COLLAPSE (the 9-brick course-15 back-eaves cut), DEEPBAND (back
 * wall courses 6-7 removed). Asserts only that nothing is Stranded anywhere (DESIGN.md §4) and,
 * as a control, that the uncut shell stands at the shipped strength; any flip is a finding.
 *
 * Outside the DestructionGame suite: run `Automation RunTests ShedRealisticLoads`. No ticking
 * world. Named namespace because of unity builds.
 */
namespace ShedRealisticMortarSensitivityProbeSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// Back-wall geometry, as in the collapse and undermine probes.
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

	// Live pieces that are neither Grounded nor Supported.
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
	 * Set every bed (brick-brick, vertical normal, as BuildRealistic classifies) to the given
	 * cohesion and tensile, keeping compressive 10.0, friction 0.75 and shear ceiling 2.0.
	 * Returns how many beds were changed.
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

	// The nine back-eaves (course 15) bricks: the collapse cut.
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

	// The full-width back-wall band, courses 6 and 7.
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

	// The result of one (strength, scenario) run.
	struct FRun
	{
		int32 Beds = 0;
		int32 Removed = 0;
		int32 Passes = 0;
		int32 LostEarth = 0;
		int32 Stranded = 0;
		bool bStands = true;
	};

	// Build fresh, re-author beds, apply the scenario's cut, settle, tally.
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

/** Sweep the bed mortar from 0.9/0.7 to 0.3/0.25 across the three scenarios; see the file header. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShedRealisticMortarSensitivityProbe,
	"ShedRealisticLoads.Probe.MortarSensitivity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FShedRealisticMortarSensitivityProbe::RunTest(const FString& Parameters)
{
	using namespace ShedRealisticMortarSensitivityProbeSupport;

	// Bed-mortar ladder; cohesion and tensile move together, as in the test campaigns.
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

			UE_LOG(LogTemp, Display,
				TEXT("SHEDMORTARSENS,%.4g,%.4g,%s,passes=%d,removed=%d,beds=%d,lostearth=%d,stranded=%d,stands=%d,note=<%s>"),
				St.Cohesion, St.Tensile, ScenarioName(Scen),
				R.Passes, R.Removed, R.Beds, R.LostEarth, R.Stranded, R.bStands ? 1 : 0, St.Note);

			// A stranded piece means the router declined rather than support genuinely being lost (DESIGN.md §4).
			TestEqual(
				*FString::Printf(
					TEXT("nothing may be Stranded at bed %.4g/%.4g, scenario %s (got %d)"),
					St.Cohesion, St.Tensile, ScenarioName(Scen), R.Stranded),
				R.Stranded, 0);

			// Control: the uncut shell stands at the shipped strength, so a lower-rung fall is a real finding.
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

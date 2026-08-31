// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A DIAGNOSTIC PROBE, NOT A PERMANENT RED. This measures the ROUTER solve time at the realistic
 * shed's ~442 blocks and prints what the ROUTER does when a porch post is pulled (case B) and when
 * the door piers are pulled (case A) — the fast half of the slice-4 authority decision. It ASSERTS
 * nothing about wall-clock (that would flake on a shared machine); it exists to be read. The name
 * deliberately omits "DestructionGame" so the full suite never runs it; invoke it with
 * `Automation RunTests ShedRealisticLatency`.
 *
 * THE LP HALF WAS RETIRED. A single LP feasibility solve at 442 blocks ran for over twenty minutes
 * (measured, still unfinished — the LP is super-linear and the promotable band was ~84-104 blocks),
 * which is the recorded number that rules the LP out and puts the router in charge at this scale; a
 * >20-minute opt-in test is dead weight, so the LpSolveAt442 probe that produced it was deleted.
 *
 * NEEDS A TICKING WORLD: NO. Boxes, doubles and the router; gravity on. No Chaos, no tick.
 */
namespace ShedRealisticLatencyProbeSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** The porch posts are the only grounded Timber in the shed. */
	void FindGroundedTimber(const FBrickLayout& L, TArray<int32>& Out)
	{
		const FStructure& S = L.Structure;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (!S.IsPieceRemoved(P) && S.GetPiece(P).Material == &Timber && S.GetPiece(P).bIsGrounded)
			{
				Out.Add(P);
			}
		}
	}

	bool SharesJoint(const FStructure& S, int32 A, int32 B)
	{
		for (int32 J = 0; J < S.NumConnections(); ++J)
		{
			const FConnection& C = S.GetConnection(J);
			if ((C.PieceA == A && C.PieceB == B) || (C.PieceA == B && C.PieceB == A))
			{
				return true;
			}
		}
		return false;
	}

	/** The free Timber board jointed to every post — the overhang. */
	int32 FindOverhang(const FBrickLayout& L, const TArray<int32>& Posts)
	{
		const FStructure& S = L.Structure;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (S.IsPieceRemoved(P) || S.GetPiece(P).Material != &Timber
				|| S.GetPiece(P).bIsGrounded || !L.Boxes.IsValidIndex(P))
			{
				continue;
			}
			bool bAll = Posts.Num() > 0;
			for (const int32 Post : Posts)
			{
				if (!SharesJoint(S, P, Post))
				{
					bAll = false;
					break;
				}
			}
			if (bAll)
			{
				return P;
			}
		}
		return INDEX_NONE;
	}

	int32 PieceContaining(const FBrickLayout& L, const FVector& Pt)
	{
		const FStructure& S = L.Structure;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (S.IsPieceRemoved(P) || !L.Boxes.IsValidIndex(P))
			{
				continue;
			}
			const FVector Lo = L.Boxes[P].CentreCm - L.Boxes[P].ExtentCm;
			const FVector Hi = L.Boxes[P].CentreCm + L.Boxes[P].ExtentCm;
			if (Pt.X >= Lo.X && Pt.X <= Hi.X && Pt.Y >= Lo.Y && Pt.Y <= Hi.Y && Pt.Z >= Lo.Z && Pt.Z <= Hi.Z)
			{
				return P;
			}
		}
		return INDEX_NONE;
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

	int32 LostEarthCount(const FStructure& S)
	{
		int32 N = 0;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (S.IsPieceRemoved(P))
			{
				continue;
			}
			const EPieceSupport Sup = S.GetPieceSupport(P);
			if (Sup != EPieceSupport::Grounded && Sup != EPieceSupport::Supported)
			{
				++N;
			}
		}
		return N;
	}

	const TCHAR* SupportName(EPieceSupport S)
	{
		switch (S)
		{
			case EPieceSupport::Grounded:  return TEXT("Grounded");
			case EPieceSupport::Supported: return TEXT("Supported");
			case EPieceSupport::Stranded:  return TEXT("Stranded");
			default:                       return TEXT("Falling");
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShedRealisticRouterProbe,
	"ShedRealisticLatency.Probe.RouterAndCasesAt442",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FShedRealisticRouterProbe::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace DestructionLayout;
	using namespace ShedRealisticLatencyProbeSupport;

	/* -------- STEP 1a: ROUTER solve time at 442 (the default path, cap 200 < 442). -------- */
	{
		FBrickLayout L;
		if (!DestructionShed3D::BuildRealistic(L))
		{
			AddError(TEXT("BuildRealistic returned false"));
			return false;
		}

		AddInfo(FString::Printf(TEXT("BUILD: %d pieces, %d joints."),
			L.Structure.NumPieces(), L.Structure.NumConnections()));

		const double T0 = FPlatformTime::Seconds();
		const int32 Passes = L.Structure.SolveAndBreak();
		const double T1 = FPlatformTime::Seconds();

		AddInfo(FString::Printf(
			TEXT("ROUTER @ %d blocks: SolveAndBreak %.1f ms, %d pass(es), %d stranded, %d lost-earth."),
			L.Structure.NumPieces(), (T1 - T0) * 1000.0, Passes,
			StrandedCount(L.Structure), LostEarthCount(L.Structure)));
	}

	/* -------- STEP 2, CASE B: pull a porch POST, router authority — does the overhang strand/fall? -------- */
	{
		FBrickLayout L;
		DestructionShed3D::BuildRealistic(L);

		TArray<int32> Posts;
		FindGroundedTimber(L, Posts);
		const int32 Overhang = FindOverhang(L, Posts);

		AddInfo(FString::Printf(TEXT("CASE B: %d grounded-timber post(s), overhang piece=%d."),
			Posts.Num(), Overhang));

		if (Posts.Num() >= 1 && Overhang != INDEX_NONE)
		{
			const int32 PulledPost = Posts[0];
			const EPieceSupport OverBefore = L.Structure.GetPieceSupport(Overhang);
			L.Structure.RemovePiece(PulledPost);

			const int32 Passes = L.Structure.SolveAndBreak();
			const EPieceSupport OverAfter = L.Structure.GetPieceSupport(Overhang);

			AddInfo(FString::Printf(
				TEXT("CASE B (router): pulled post %d; overhang %d before=%s after=%s; %d pass(es), "
					 "%d stranded, %d lost-earth. (The ROUTER over-holds if the overhang reads Supported.)"),
				PulledPost, Overhang, SupportName(OverBefore), SupportName(OverAfter),
				Passes, StrandedCount(L.Structure), LostEarthCount(L.Structure)));
		}
	}

	/* -------- STEP 2, CASE A: pull the door PIERS, router authority — does the door head lose the earth?
	 * Two candidate cuts, each measured precisely with the exact filter the collapse test will use. -------- */
	for (int32 Candidate = 0; Candidate < 2; ++Candidate)
	{
		FBrickLayout L;
		DestructionShed3D::BuildRealistic(L);

		const FStructure& CS = L.Structure;
		TArray<int32> Cut;
		for (int32 P = 0; P < CS.NumPieces(); ++P)
		{
			if (CS.IsPieceRemoved(P) || CS.GetPiece(P).Material != &ClayBrick || !L.Boxes.IsValidIndex(P))
			{
				continue;
			}
			const FVector C = L.Boxes[P].CentreCm;
			const bool bFrontBand = FMath::Abs(C.Y - 5.125) < 1.0;   // front wall thin band
			bool bTake = false;
			if (Candidate == 0)
			{
				/* (0) THE WHOLE DOOR PIERS below the lintel: both flanks of the door, courses 0..11. */
				const bool bLowCourses = C.Z < 89.5;
				const bool bLeftPier = C.X < 57.5;
				const bool bRightPier = C.X > 122.5;
				bTake = bFrontBand && bLowCourses && (bLeftPier || bRightPier);
			}
			else
			{
				/* (1) ONLY the two course-11 pier tops the lintel bears on: Z centre ~85.25, edges at the
				 * door (left brick [33.75,55.25], right [123.75,145.25]). */
				const bool bCourse11 = FMath::Abs(C.Z - 85.25) < 1.0;
				const bool bLeftBearing = C.X > 33.0 && C.X < 56.0;
				const bool bRightBearing = C.X > 123.0 && C.X < 146.0;
				bTake = bFrontBand && bCourse11 && (bLeftBearing || bRightBearing);
			}
			if (bTake)
			{
				Cut.Add(P);
			}
		}

		const int32 DoorLintel = PieceContaining(L, FVector(90.0, 5.0, 93.0));
		for (const int32 P : Cut)
		{
			L.Structure.RemovePiece(P);
		}

		const int32 Passes = L.Structure.SolveAndBreak();
		const EPieceSupport LintelAfter =
			DoorLintel != INDEX_NONE ? L.Structure.GetPieceSupport(DoorLintel) : EPieceSupport::Falling;

		AddInfo(FString::Printf(
			TEXT("CASE A cand %d (router): removed %d brick(s); door lintel %d after=%s; "
				 "%d pass(es), %d stranded, %d lost-earth."),
			Candidate, Cut.Num(), DoorLintel, SupportName(LintelAfter),
			Passes, StrandedCount(L.Structure), LostEarthCount(L.Structure)));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

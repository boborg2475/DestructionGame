// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Diagnostic probe: router solve time on the ~442-block realistic shed, and what it does when a
 * porch post (case B) or the door piers (case A) are pulled. Prints only; wall-clock asserts would
 * flake. Omitted from the full suite by name; run `Automation RunTests ShedRealisticLatency`.
 *
 * The LP probe was removed: one LP solve at 442 blocks ran over 20 minutes without finishing,
 * which is why the router is the authority at this scale. No world.
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

	/*
	 * Back-wall bricks (Y = 128.875) in courses CourseLo..CourseHi with X centre in [XLoCm, XHiCm].
	 * Course c is centred at Z = c * 7.5 + 3.25.
	 */
	void CollectBackWallBand(
		const FBrickLayout& L, int32 CourseLo, int32 CourseHi, double XLoCm, double XHiCm, TArray<int32>& Out)
	{
		const FStructure& S = L.Structure;
		const double BackWallYCm = 128.875;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (S.IsPieceRemoved(P) || S.GetPiece(P).Material != &ClayBrick || !L.Boxes.IsValidIndex(P))
			{
				continue;
			}
			const FVector C = L.Boxes[P].CentreCm;
			const bool bBackWall = FMath::Abs(C.Y - BackWallYCm) < 1.0;
			bool bInBand = false;
			for (int32 Course = CourseLo; Course <= CourseHi; ++Course)
			{
				if (FMath::Abs(C.Z - (Course * 7.5 + 3.25)) < 1.0)
				{
					bInBand = true;
					break;
				}
			}
			if (bBackWall && bInBand && C.X >= XLoCm && C.X <= XHiCm)
			{
				Out.Add(P);
			}
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

	// Router solve time at 442 blocks (above the 200-block cap, so the router path).
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

	// Case B: pull a porch post; does the overhang lose support?
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

	// Case A: pull the door piers (two candidate cuts); does the door lintel lose support?
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
				// (0) Both door piers below the lintel, courses 0..11.
				const bool bLowCourses = C.Z < 89.5;
				const bool bLeftPier = C.X < 57.5;
				const bool bRightPier = C.X > 122.5;
				bTake = bFrontBand && bLowCourses && (bLeftPier || bRightPier);
			}
			else
			{
				// (1) Only the two course-11 bricks the lintel bears on (Z ~85.25).
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

/**
 * Diagnostic: cut a 2-course band from the back wall at several widths and print whether the
 * router arches over the gap or drops the masonry above. Asserts nothing. No world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShedRealisticLowBandProbe,
	"ShedRealisticLatency.Probe.LowBandArchVsCollapse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FShedRealisticLowBandProbe::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace DestructionLayout;
	using namespace ShedRealisticLatencyProbeSupport;

	// Each case runs on a fresh build.
	struct FCase { const TCHAR* Name; int32 CourseLo; int32 CourseHi; double XLo; double XHi; };
	const FCase Cases[] = {
		{ TEXT("LOW  NARROW (c6-7, central ~3 cols, X 78..101)"), 6, 7, 78.0, 101.0 },
		{ TEXT("LOW  WIDE   (c6-7, ends kept, X 22..158)"), 6, 7, 22.0, 158.0 },
		{ TEXT("LOW  FULL   (c6-7, whole low band, X 0..180)"), 6, 7, 0.0, 180.0 },
		{ TEXT("HIGH FULL   (c13-14 under eaves, whole band, X 0..180)"), 13, 14, 0.0, 180.0 },
	};

	for (const FCase& Case : Cases)
	{
		FBrickLayout L;
		if (!DestructionShed3D::BuildRealistic(L))
		{
			AddError(TEXT("BuildRealistic returned false"));
			return false;
		}

		TArray<int32> Band;
		CollectBackWallBand(L, Case.CourseLo, Case.CourseHi, Case.XLo, Case.XHi, Band);

		int32 Removed = 0;
		for (const int32 P : Band)
		{
			if (L.Structure.RemovePiece(P))
			{
				++Removed;
			}
		}

		const int32 Passes = L.Structure.SolveAndBreak();
		const int32 Stranded = StrandedCount(L.Structure);
		const int32 LostEarth = LostEarthCount(L.Structure);

		AddInfo(FString::Printf(
			TEXT("LOW-BAND %s: removed %d brick(s); %d pass(es); %d lost-earth; %d stranded."),
			Case.Name, Removed, Passes, LostEarth, Stranded));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

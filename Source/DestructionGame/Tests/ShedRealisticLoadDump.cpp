// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Load-dump diagnostic, not a test. Builds the realistic shed (~442 pieces), runs the router solve
 * (SolveLoads, nothing broken) and logs per-piece and per-joint load as CSV under the SHEDLOAD_
 * prefix for a heat-map. The name omits "DestructionGame" so the full suite skips it; run with
 * `Automation RunTests ShedRealisticLoads`. No ticking world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShedRealisticLoadDump,
	"ShedRealisticLoads.Dump.StandingShed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FShedRealisticLoadDump::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace DestructionLayout;

	FBrickLayout L;
	if (!DestructionShed3D::BuildRealistic(L))
	{
		AddError(TEXT("BuildRealistic returned false"));
		return false;
	}

	FStructure& S = L.Structure;

	// Router solve only: nothing breaks, so every joint reports the intact shed's load.
	S.SolveLoads();

	UE_LOG(LogTemp, Display, TEXT("SHEDLOAD_BEGIN,%d pieces,%d joints"),
		S.NumPieces(), S.NumConnections());

	/*
	 * Piece rows: box centre and half-extent (cm); mat 1 Timber, 0 ClayBrick, -1 none; grounded 0/1;
	 * support as EPieceSupport (0 Falling, 1 Grounded, 2 Supported, 3 Stranded).
	 */
	for (int32 P = 0; P < S.NumPieces(); ++P)
	{
		if (S.IsPieceRemoved(P) || !L.Boxes.IsValidIndex(P))
		{
			continue;
		}

		const FVector C = L.Boxes[P].CentreCm;
		const FVector E = L.Boxes[P].ExtentCm;
		const DestructionProfiles::FMaterialProfile* Mat = S.GetPiece(P).Material;
		const int32 MatCode = (Mat == &Timber) ? 1 : (Mat == &ClayBrick) ? 0 : -1;
		const int32 Grounded = S.GetPiece(P).bIsGrounded ? 1 : 0;
		const int32 Support = static_cast<int32>(S.GetPieceSupport(P));

		UE_LOG(LogTemp, Display,
			TEXT("SHEDLOAD_PIECE,%d,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%d,%d,%d"),
			P, C.X, C.Y, C.Z, E.X, E.Y, E.Z, MatCode, Grounded, Support);
	}

	/*
	 * Joint rows: interface centre and normal (toward PieceB), area cm2, force magnitude and
	 * components in uu (1 N = 100 uu), moment magnitude, and utilisation.
	 */
	double MaxForce = 0.0;
	double MaxUtil = 0.0;
	for (int32 J = 0; J < S.NumConnections(); ++J)
	{
		const FConnection& Conn = S.GetConnection(J);
		const FVector Ctr = Conn.InterfaceCentreCm;
		const FVector N = Conn.InterfaceNormal;
		const FVector F = S.GetConnectionForce(J);
		const double FMag = F.Size();
		const double Moment = S.GetConnectionMoment(J).Size();
		const double Util = S.GetConnectionUtilisation(J);

		MaxForce = FMath::Max(MaxForce, FMag);
		if (Util != TNumericLimits<double>::Max())
		{
			MaxUtil = FMath::Max(MaxUtil, Util);
		}

		UE_LOG(LogTemp, Display,
			TEXT("SHEDLOAD_JOINT,%d,%d,%d,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g"),
			J, Conn.PieceA, Conn.PieceB, Ctr.X, Ctr.Y, Ctr.Z, N.X, N.Y, N.Z,
			Conn.InterfaceAreaSqCm, FMag, F.X, F.Y, F.Z, Moment, Util);
	}

	/*
	 * In equilibrium the ground reaction equals total weight, sum of MassKg * 980 in uu (980 already
	 * includes the 1 N = 100 uu conversion; see Structure.h).
	 */
	double GroundReaction = 0.0;
	for (int32 P = 0; P < S.NumPieces(); ++P)
	{
		if (!S.IsPieceRemoved(P))
		{
			GroundReaction += S.GetPiece(P).MassKg * 980.0;
		}
	}

	UE_LOG(LogTemp, Display,
		TEXT("SHEDLOAD_SUMMARY,pieces=%d,joints=%d,maxforce=%.6g,maxutil=%.6g,groundreaction=%.6g"),
		S.NumPieces(), S.NumConnections(), MaxForce, MaxUtil, GroundReaction);

	return true;
}

/**
 * The same dump with the low back-wall band (courses 6 and 7, 17 bricks) removed first, showing how
 * load re-routes around the gap. Same log format so the two dumps overlay; removed pieces print with
 * support = -9. Not in the full suite; no ticking world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShedRealisticLoadDumpLowBandRemoved,
	"ShedRealisticLoads.Dump.LowBandRemoved",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FShedRealisticLoadDumpLowBandRemoved::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace DestructionLayout;

	FBrickLayout L;
	if (!DestructionShed3D::BuildRealistic(L))
	{
		AddError(TEXT("BuildRealistic returned false"));
		return false;
	}

	FStructure& S = L.Structure;

	/*
	 * The band: ClayBrick pieces at Y = 128.875 in courses 6 and 7 (Z = c * 7.5 + 3.25), 8 + 9 bricks.
	 * LowBandArchVsCollapse found it stands with 0 lost-earth.
	 */
	const double BackWallYCm = 128.875;
	TArray<int32> Band;
	for (int32 P = 0; P < S.NumPieces(); ++P)
	{
		if (S.IsPieceRemoved(P) || S.GetPiece(P).Material != &ClayBrick || !L.Boxes.IsValidIndex(P))
		{
			continue;
		}
		const FVector C = L.Boxes[P].CentreCm;
		const bool bBackWall = FMath::Abs(C.Y - BackWallYCm) < 1.0;
		const bool bCourse6 = FMath::Abs(C.Z - (6 * 7.5 + 3.25)) < 1.0;
		const bool bCourse7 = FMath::Abs(C.Z - (7 * 7.5 + 3.25)) < 1.0;
		if (bBackWall && (bCourse6 || bCourse7))
		{
			Band.Add(P);
		}
	}

	int32 Removed = 0;
	for (const int32 P : Band)
	{
		if (S.RemovePiece(P))
		{
			++Removed;
		}
	}

	S.SolveLoads();

	UE_LOG(LogTemp, Display, TEXT("SHEDLOAD_BEGIN,%d pieces,%d joints,%d removed"),
		S.NumPieces(), S.NumConnections(), Removed);

	// Same columns as the standing dump; removed pieces still print, with support = -9.
	for (int32 P = 0; P < S.NumPieces(); ++P)
	{
		if (!L.Boxes.IsValidIndex(P))
		{
			continue;
		}

		const FVector C = L.Boxes[P].CentreCm;
		const FVector E = L.Boxes[P].ExtentCm;
		const DestructionProfiles::FMaterialProfile* Mat = S.GetPiece(P).Material;
		const int32 MatCode = (Mat == &Timber) ? 1 : (Mat == &ClayBrick) ? 0 : -1;
		const bool bRemoved = S.IsPieceRemoved(P);
		const int32 Grounded = (!bRemoved && S.GetPiece(P).bIsGrounded) ? 1 : 0;
		const int32 Support = bRemoved ? -9 : static_cast<int32>(S.GetPieceSupport(P));

		UE_LOG(LogTemp, Display,
			TEXT("SHEDLOAD_PIECE,%d,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%d,%d,%d"),
			P, C.X, C.Y, C.Z, E.X, E.Y, E.Z, MatCode, Grounded, Support);
	}

	// Joints of removed pieces read zero force; kept so the two dumps stay index-aligned.
	double MaxForce = 0.0;
	double MaxUtil = 0.0;
	for (int32 J = 0; J < S.NumConnections(); ++J)
	{
		const FConnection& Conn = S.GetConnection(J);
		const FVector Ctr = Conn.InterfaceCentreCm;
		const FVector N = Conn.InterfaceNormal;
		const FVector F = S.GetConnectionForce(J);
		const double FMag = F.Size();
		const double Moment = S.GetConnectionMoment(J).Size();
		const double Util = S.GetConnectionUtilisation(J);

		MaxForce = FMath::Max(MaxForce, FMag);
		if (Util != TNumericLimits<double>::Max())
		{
			MaxUtil = FMath::Max(MaxUtil, Util);
		}

		UE_LOG(LogTemp, Display,
			TEXT("SHEDLOAD_JOINT,%d,%d,%d,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%.4g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g"),
			J, Conn.PieceA, Conn.PieceB, Ctr.X, Ctr.Y, Ctr.Z, N.X, N.Y, N.Z,
			Conn.InterfaceAreaSqCm, FMag, F.X, F.Y, F.Z, Moment, Util);
	}

	// Survivors neither Grounded nor Supported; the band arches iff this is 0.
	int32 LostEarth = 0;
	double GroundReaction = 0.0;
	for (int32 P = 0; P < S.NumPieces(); ++P)
	{
		if (S.IsPieceRemoved(P))
		{
			continue;
		}
		const EPieceSupport Sup = S.GetPieceSupport(P);
		if (Sup != EPieceSupport::Grounded && Sup != EPieceSupport::Supported)
		{
			++LostEarth;
		}
		GroundReaction += S.GetPiece(P).MassKg * 980.0;
	}

	UE_LOG(LogTemp, Display,
		TEXT("SHEDLOAD_SUMMARY,pieces=%d,joints=%d,maxforce=%.6g,maxutil=%.6g,groundreaction=%.6g,removed=%d,lostearth=%d"),
		S.NumPieces(), S.NumConnections(), MaxForce, MaxUtil, GroundReaction, Removed, LostEarth);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

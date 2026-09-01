// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A LOAD-DUMP DIAGNOSTIC, NOT A PERMANENT RED. It builds the standing realistic brick shed
 * (~442 pieces), runs the ROUTER load solve (SolveLoads — nothing removed, nothing broken),
 * and LOGS the per-piece and per-joint load in a machine-parseable CSV so the orchestrator
 * can build a load heat-map. It ASSERTS almost nothing — it exists to be grepped out of
 * Saved/Logs/DestructionGame.log under the SHEDLOAD_ prefix. The name deliberately omits
 * "DestructionGame" so the full suite never runs it; invoke it with
 * `Automation RunTests ShedRealisticLoads`.
 *
 * NEEDS A TICKING WORLD: NO. Boxes, doubles and the router; gravity on. No Chaos, no tick.
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

	/*
	 * The ROUTER load solve, and ONLY that: nothing is removed and nothing breaks, so every
	 * joint reports what the intact standing shed puts through it. GetConnectionForce /
	 * GetConnectionMoment / GetConnectionUtilisation / GetPieceSupport all read this solve.
	 */
	S.SolveLoads();

	UE_LOG(LogTemp, Display, TEXT("SHEDLOAD_BEGIN,%d pieces,%d joints"),
		S.NumPieces(), S.NumConnections());

	/*
	 * PER-PIECE ROWS. cx/cy/cz is the box centre and ex/ey/ez the box HALF-extent, both cm,
	 * straight off the layout's parallel Boxes array. mat is 1 for Timber, 0 for ClayBrick,
	 * -1 for a piece that names no material (none in this shed). grounded is 0/1 and support
	 * is the EPieceSupport int (0 Falling, 1 Grounded, 2 Supported, 3 Stranded).
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
	 * PER-JOINT ROWS. jx/jy/jz is the joint's contact centre — the producer's overlap-rectangle
	 * centre (FConnection::InterfaceCentreCm) — and nx/ny/nz its interface normal (pointing
	 * toward PieceB). area is the contact area cm2. fmag is |GetConnectionForce| with its
	 * components fx/fy/fz (Unreal force units, 1 N = 100 uu), moment is |GetConnectionMoment|,
	 * and util is GetConnectionUtilisation on the 0 -> 1 -> >1 scale.
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
	 * THE GROUND REACTION IS THE STANDING STRUCTURE'S TOTAL WEIGHT. A structure in static
	 * equilibrium pushes exactly its own weight into the earth, so summing MassKg * 980 over
	 * every live piece is the total vertical reaction without having to attribute it joint by
	 * joint — the 980 already IS the 1 N = 100 uu conversion (see Structure.h's UNITS note).
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
 * THE SAME LOAD DUMP AS ABOVE, BUT WITH THE LOW BACK-WALL BAND REMOVED FIRST — the "arching" state.
 * Courses 6 and 7 of the +Y back wall (Z centres 48.25 / 55.75, the full-width low band, 17 bricks)
 * are pulled before the solve, so the dump shows how load re-routes around a low gap the running-bond
 * wall deep-beams over. It mirrors ShedRealisticLoads.Dump.StandingShed byte-for-byte in log format so
 * the two dumps overlay: a REMOVED piece still prints its box centre/extent (so the slot's location is
 * known) but carries support=-9, a sentinel the heat-map draws as empty. Joints incident to a removed
 * piece read fmag 0 — expected. It ASSERTS almost nothing; grep it out under the SHEDLOAD_ prefix. The
 * name omits "DestructionGame" so the full suite never runs it; invoke with
 * `Automation RunTests ShedRealisticLoads`.
 *
 * NEEDS A TICKING WORLD: NO. Boxes, doubles and the router; gravity on. No Chaos, no tick.
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
	 * PULL THE LOW BACK-WALL BAND: courses 6 and 7 of the +Y back wall, the full-width low band (17
	 * bricks — 8 in the even course 6 at Z 48.25, 9 in the odd course 7 at Z 55.75). A brick is taken
	 * if it is ClayBrick, sits in the back-wall Y band centred on 128.875, and its Z centre matches one
	 * of the two course centres (c * 7.5 + 3.25). This is the same band the LowBandArchVsCollapse probe
	 * found stands with 0 lost-earth.
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

	/*
	 * The ROUTER load solve on the arched structure. The shed still stands (the earlier finding was 0
	 * lost-earth for this full-width low band), so every survivor should read Grounded or Supported and
	 * the joints report how the load routes around the slot.
	 */
	S.SolveLoads();

	UE_LOG(LogTemp, Display, TEXT("SHEDLOAD_BEGIN,%d pieces,%d joints,%d removed"),
		S.NumPieces(), S.NumConnections(), Removed);

	/*
	 * PER-PIECE ROWS, identical columns to the standing dump. A REMOVED piece is NOT skipped: it still
	 * prints its box centre/extent so the slot's location is known, but its support field is forced to
	 * the -9 removed sentinel. Live pieces print the real EPieceSupport int (0 Falling, 1 Grounded,
	 * 2 Supported, 3 Stranded).
	 */
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

	/*
	 * PER-JOINT ROWS, identical columns to the standing dump. Joints incident to a removed piece read
	 * fmag 0 because the tombstoned piece carries no load — that is expected and left in so the two
	 * dumps stay index-aligned.
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
	 * HOW MANY SURVIVORS LOST THE EARTH — a live piece reading anything other than Grounded or Supported
	 * is one the arch failed to carry. The band arches iff this is 0.
	 */
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

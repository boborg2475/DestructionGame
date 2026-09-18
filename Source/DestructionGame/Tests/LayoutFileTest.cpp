// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/ContactSweep.h"
#include "Core/Layout.h"
#include "Core/LayoutFile.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE LAYOUT FILE — a structure as DATA (Core/LayoutFile.h), the owner's 2026-09-18 ruling that a
 * building is a file, not a C++ builder, because players will build in the game without C++.
 *
 * Three behaviours: (1) a layout ROUND-TRIPS — the pieces, their boxes, materials and grounding,
 * the 3D flag, and the SAME joints (because the joints are swept from the boxes on both sides, a
 * round trip that changed a joint would be a box that changed); (2) bad input is REFUSED and leaves
 * the layout EMPTY, never half-read; (3) the warehouse's checked-in file LOADS, is the size the
 * design says, is 3D, has no two boxes overlapping, and has no piece floating — the geometric
 * safety net for a five-thousand-piece file nobody can read by eye.
 *
 * NEEDS A TICKING WORLD: NO. Strings, boxes and the sweep; the warehouse test reads one file.
 */
namespace LayoutFileTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** A box from its corners, with the material recorded — what every builder's AddPiece does. */
	int32 LayoutFileAddPiece(
		FBrickLayout& Layout, const FVector& MinCm, const FVector& MaxCm,
		const FMaterialProfile& Material, bool bGrounded)
	{
		FPieceBox Box;
		Box.CentreCm = (MinCm + MaxCm) / 2.0;
		Box.ExtentCm = (MaxCm - MinCm) / 2.0;

		const int32 Piece = Layout.Structure.AddPiece(
			PieceMassKg(Box, Material.DensityGramsPerCubicCm), bGrounded, Box.CentreCm);

		Layout.Boxes.Add(Box);
		Layout.Structure.SetPieceMaterial(Piece, &Material);

		return Piece;
	}

	/*
	 * A SMALL MIXED-MATERIAL STRUCTURE: two grounded bricks side by side (a head joint), a brick
	 * bridging them one joint up (two beds), a stone block beside that brick (a head joint), and a
	 * timber plate across the top (dry bearings). Every kind of contact the sweep classifies.
	 */
	void LayoutFileLaySample(FBrickLayout& Layout)
	{
		LayoutFileAddPiece(Layout, FVector(0.0, 0.0, 0.0), FVector(21.5, 10.25, 6.5), ClayBrick, true);
		LayoutFileAddPiece(Layout, FVector(22.5, 0.0, 0.0), FVector(44.0, 10.25, 6.5), ClayBrick, true);
		LayoutFileAddPiece(Layout, FVector(11.25, 0.0, 7.5), FVector(32.75, 10.25, 14.0), ClayBrick, false);
		LayoutFileAddPiece(Layout, FVector(33.75, 0.0, 7.5), FVector(44.0, 10.25, 14.0), StructuralConcrete, false);
		LayoutFileAddPiece(Layout, FVector(0.0, 0.0, 15.0), FVector(44.0, 10.25, 20.0), Timber, false);
		Layout.Structure.SetThreeDimensional(true);
	}

	/** Exhaustive strict-AABB overlap count, bucketed on Z so a big file finishes. */
	int32 LayoutFileOverlappingPairs(const FBrickLayout& Layout)
	{
		const double Eps = 1.0e-6;
		const double BandCm = 15.0;
		TMap<int32, TArray<int32>> Buckets;

		for (int32 Piece = 0; Piece < Layout.Boxes.Num(); ++Piece)
		{
			const FPieceBox& Box = Layout.Boxes[Piece];
			const int32 First = FMath::FloorToInt32((Box.CentreCm.Z - Box.ExtentCm.Z) / BandCm);
			const int32 Last = FMath::FloorToInt32((Box.CentreCm.Z + Box.ExtentCm.Z - Eps) / BandCm);

			for (int32 Bucket = First; Bucket <= Last; ++Bucket)
			{
				Buckets.FindOrAdd(Bucket).Add(Piece);
			}
		}

		TSet<uint64> Seen;
		int32 Overlaps = 0;

		for (const TPair<int32, TArray<int32>>& Bucket : Buckets)
		{
			const TArray<int32>& List = Bucket.Value;

			for (int32 I = 0; I < List.Num(); ++I)
			{
				for (int32 J = I + 1; J < List.Num(); ++J)
				{
					const int32 A = FMath::Min(List[I], List[J]);
					const int32 B = FMath::Max(List[I], List[J]);
					const uint64 Key = (static_cast<uint64>(A) << 32) | static_cast<uint32>(B);

					if (Seen.Contains(Key))
					{
						continue;
					}

					Seen.Add(Key);

					const FVector LoA = Layout.Boxes[A].CentreCm - Layout.Boxes[A].ExtentCm;
					const FVector HiA = Layout.Boxes[A].CentreCm + Layout.Boxes[A].ExtentCm;
					const FVector LoB = Layout.Boxes[B].CentreCm - Layout.Boxes[B].ExtentCm;
					const FVector HiB = Layout.Boxes[B].CentreCm + Layout.Boxes[B].ExtentCm;

					if (LoA.X < HiB.X - Eps && HiA.X > LoB.X + Eps
						&& LoA.Y < HiB.Y - Eps && HiA.Y > LoB.Y + Eps
						&& LoA.Z < HiB.Z - Eps && HiA.Z > LoB.Z + Eps)
					{
						++Overlaps;
					}
				}
			}
		}

		return Overlaps;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLayoutFileRoundTripTest,
	"DestructionGame.Core.LayoutFile.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLayoutFileRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace LayoutFileTestSupport;

	FBrickLayout Original;
	LayoutFileLaySample(Original);
	TestTrue(TEXT("the sample's joints sweep"), SweepContacts(Original, 1.0));

	const FString Json = DestructionLayoutFile::Serialize(Original, 1.0);
	TestTrue(TEXT("the text names the format"), Json.Contains(TEXT("\"format\": \"DestructionGame.Layout\"")));
	TestTrue(TEXT("the text names a material"), Json.Contains(TEXT("\"material\": \"StructuralConcrete\"")));

	FBrickLayout Read;
	FString Why;
	TestTrue(FString::Printf(TEXT("the text parses back (%s)"), *Why), DestructionLayoutFile::Parse(Json, Read, &Why));

	TestEqual(TEXT("piece count"), Read.Structure.NumPieces(), Original.Structure.NumPieces());
	TestEqual(TEXT("box count"), Read.Boxes.Num(), Original.Boxes.Num());
	TestEqual(TEXT("joint count"), Read.Structure.NumConnections(), Original.Structure.NumConnections());
	TestTrue(TEXT("the 3D flag survives"), Read.Structure.IsThreeDimensional());

	for (int32 Piece = 0; Piece < FMath::Min(Read.Boxes.Num(), Original.Boxes.Num()); ++Piece)
	{
		TestTrue(
			FString::Printf(TEXT("piece %d centre"), Piece),
			Read.Boxes[Piece].CentreCm.Equals(Original.Boxes[Piece].CentreCm, 1.0e-6));
		TestTrue(
			FString::Printf(TEXT("piece %d extent"), Piece),
			Read.Boxes[Piece].ExtentCm.Equals(Original.Boxes[Piece].ExtentCm, 1.0e-6));
		TestEqual(
			FString::Printf(TEXT("piece %d material"), Piece),
			Read.Structure.GetPiece(Piece).Material, Original.Structure.GetPiece(Piece).Material);
		TestEqual(
			FString::Printf(TEXT("piece %d grounded"), Piece),
			Read.Structure.GetPiece(Piece).bIsGrounded, Original.Structure.GetPiece(Piece).bIsGrounded);
		TestTrue(
			FString::Printf(TEXT("piece %d mass"), Piece),
			FMath::IsNearlyEqual(Read.Structure.GetPiece(Piece).MassKg, Original.Structure.GetPiece(Piece).MassKg, 1.0e-9));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLayoutFileRefusalTest,
	"DestructionGame.Core.LayoutFile.RefusesBadInputAndLeavesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLayoutFileRefusalTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;

	const TCHAR* const Head =
		TEXT("{ \"format\": \"DestructionGame.Layout\", \"version\": 1, \"jointThicknessCm\": 1, \"pieces\": [");
	const TCHAR* const GoodPiece =
		TEXT("{ \"min\": [0, 0, 0], \"max\": [21.5, 10.25, 6.5], \"material\": \"ClayBrick\", \"grounded\": true }");

	const TPair<const TCHAR*, FString> Cases[] = {
		{ TEXT("empty text"), FString() },
		{ TEXT("not JSON"), FString(TEXT("bricks")) },
		{ TEXT("wrong format name"),
			FString(TEXT("{ \"format\": \"Something.Else\", \"version\": 1, \"jointThicknessCm\": 1, \"pieces\": [")) + GoodPiece + TEXT("] }") },
		{ TEXT("wrong version"),
			FString(TEXT("{ \"format\": \"DestructionGame.Layout\", \"version\": 2, \"jointThicknessCm\": 1, \"pieces\": [")) + GoodPiece + TEXT("] }") },
		{ TEXT("no pieces"), FString(Head) + TEXT("] }") },
		{ TEXT("unknown material"),
			FString(Head) + GoodPiece + TEXT(", { \"min\": [0, 0, 7.5], \"max\": [21.5, 10.25, 14], \"material\": \"Cheese\" }] }") },
		{ TEXT("degenerate box"),
			FString(Head) + GoodPiece + TEXT(", { \"min\": [0, 0, 7.5], \"max\": [0, 10.25, 14], \"material\": \"ClayBrick\" }] }") },
		{ TEXT("missing box"),
			FString(Head) + GoodPiece + TEXT(", { \"material\": \"ClayBrick\" }] }") },
	};

	for (const TPair<const TCHAR*, FString>& Case : Cases)
	{
		FBrickLayout Layout;
		Layout.Boxes.Add(FPieceBox());   // something to be emptied
		FString Why;

		const bool bParsed = DestructionLayoutFile::Parse(Case.Value, Layout, &Why);

		TestFalse(FString::Printf(TEXT("%s is refused"), Case.Key), bParsed);
		TestTrue(FString::Printf(TEXT("%s says why"), Case.Key), !Why.IsEmpty());
		TestEqual(FString::Printf(TEXT("%s leaves no pieces"), Case.Key), Layout.Structure.NumPieces(), 0);
		TestEqual(FString::Printf(TEXT("%s leaves no boxes"), Case.Key), Layout.Boxes.Num(), 0);
	}

	/* And the good piece alone is accepted, so the refusals above are about the faults, not the head. */
	FBrickLayout Good;
	TestTrue(TEXT("one good piece parses"), DestructionLayoutFile::Parse(FString(Head) + GoodPiece + TEXT("] }"), Good));
	TestEqual(TEXT("one good piece"), Good.Structure.NumPieces(), 1);
	TestFalse(TEXT("threeDimensional defaults to false"), Good.Structure.IsThreeDimensional());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLayoutFileWarehouseTest,
	"DestructionGame.Content.LayoutFile.WarehouseLoadsAndIsSound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLayoutFileWarehouseTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace LayoutFileTestSupport;

	/* WAREHOUSE_DESIGN.md's numbers, as the generator script produces them. */
	constexpr int32 WarehousePieces = 5612;
	constexpr int32 WarehouseTimber = 54;
	constexpr int32 WarehouseStone = 161;
	constexpr int32 WarehouseGrounded = 57;
	const FBox WarehouseEnvelopeCm(FVector(0.0, -11.25, 0.0), FVector(679.625, 347.75, 681.5));

	FBrickLayout Layout;
	FString Why;
	const double T0 = FPlatformTime::Seconds();
	const bool bLoaded = DestructionLayoutFile::LoadFile(DestructionLayoutFile::ContentPath(TEXT("Warehouse")), Layout, &Why);
	const double LoadSeconds = FPlatformTime::Seconds() - T0;

	if (!TestTrue(FString::Printf(TEXT("the warehouse file loads (%s)"), *Why), bLoaded))
	{
		return false;
	}

	AddInfo(FString::Printf(
		TEXT("warehouse: %d pieces, %d joints, loaded and swept in %.0f ms"),
		Layout.Structure.NumPieces(), Layout.Structure.NumConnections(), LoadSeconds * 1000.0));

	TestEqual(TEXT("piece count"), Layout.Structure.NumPieces(), WarehousePieces);
	TestEqual(TEXT("box count"), Layout.Boxes.Num(), WarehousePieces);
	TestTrue(TEXT("flagged 3D"), Layout.Structure.IsThreeDimensional());
	TestTrue(TEXT("loads in under ten seconds"), LoadSeconds < 10.0);

	int32 TimberCount = 0;
	int32 StoneCount = 0;
	int32 Grounded = 0;
	FBox Envelope(ForceInit);

	for (int32 Piece = 0; Piece < Layout.Boxes.Num(); ++Piece)
	{
		const FMaterialProfile* Material = Layout.Structure.GetPiece(Piece).Material;
		TimberCount += Material == &DestructionProfiles::Timber ? 1 : 0;
		StoneCount += Material == &StructuralConcrete ? 1 : 0;
		Grounded += Layout.Structure.GetPiece(Piece).bIsGrounded ? 1 : 0;
		Envelope += FBox(
			Layout.Boxes[Piece].CentreCm - Layout.Boxes[Piece].ExtentCm,
			Layout.Boxes[Piece].CentreCm + Layout.Boxes[Piece].ExtentCm);

		if (Layout.Structure.GetPiece(Piece).bIsGrounded)
		{
			TestTrue(
				FString::Printf(TEXT("grounded piece %d stands on the ground"), Piece),
				FMath::IsNearlyZero(Layout.Boxes[Piece].CentreCm.Z - Layout.Boxes[Piece].ExtentCm.Z, 1.0e-6));
		}
	}

	TestEqual(TEXT("timber pieces"), TimberCount, WarehouseTimber);
	TestEqual(TEXT("stone pieces"), StoneCount, WarehouseStone);
	TestEqual(TEXT("grounded pieces"), Grounded, WarehouseGrounded);
	TestTrue(TEXT("envelope min"), Envelope.Min.Equals(WarehouseEnvelopeCm.Min, 1.0e-6));
	TestTrue(TEXT("envelope max"), Envelope.Max.Equals(WarehouseEnvelopeCm.Max, 1.0e-6));

	TestEqual(TEXT("no two boxes overlap"), LayoutFileOverlappingPairs(Layout), 0);

	/* Nothing floats: every free piece has a joint to a piece whose centre is lower. */
	TArray<bool> HasBed;
	HasBed.SetNumZeroed(Layout.Boxes.Num());

	for (int32 Joint = 0; Joint < Layout.Structure.NumConnections(); ++Joint)
	{
		const FConnection& Connection = Layout.Structure.GetConnection(Joint);
		const int32 Lower =
			Layout.Boxes[Connection.PieceA].CentreCm.Z <= Layout.Boxes[Connection.PieceB].CentreCm.Z
				? Connection.PieceA : Connection.PieceB;
		const int32 Upper = Lower == Connection.PieceA ? Connection.PieceB : Connection.PieceA;

		if (Layout.Boxes[Lower].CentreCm.Z < Layout.Boxes[Upper].CentreCm.Z - 1.0e-6)
		{
			HasBed[Upper] = true;
		}
	}

	int32 Floating = 0;

	for (int32 Piece = 0; Piece < Layout.Boxes.Num(); ++Piece)
	{
		if (!Layout.Structure.GetPiece(Piece).bIsGrounded && !HasBed[Piece])
		{
			++Floating;
		}
	}

	TestEqual(TEXT("no free piece is without a bed beneath it"), Floating, 0);

	return true;
}

#endif

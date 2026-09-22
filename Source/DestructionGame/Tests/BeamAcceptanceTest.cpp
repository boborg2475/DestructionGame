// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Beam acceptance: a simply supported beam under a heavy weight, where the member should fail and
 * no bearing should. Covers PROJECT_REVIEW.md §2 items 3 (multi-support moment zeroed) and 7
 * (pieces never fail, only joints); evolution steps 5 and 6.
 *
 * Pieces carry only mass, so the beam is two segments whose midspan joint carries the member
 * material's own strengths; snapping is that glue line parting. Bearings and the load contacts are
 * dry. Two segments, because more leaves middle segments seatless and ApplyArchingThrust then
 * shears the beam off its bearings (H/V 7.2 vs mu 0.7 on a five-segment draft).
 *
 * Known red: each half-beam seats on its own pier, so the midspan joint reads exactly zero moment.
 * Assertions are on path-to-earth plus the glue line giving, never displacement (DESIGN.md §4).
 * Everything except Layout::MakeInterface is re-derived here, including the unit conversion. Named
 * namespace for unity builds.
 */
namespace BeamAcceptanceTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// Geometry, cm (1 uu = 1 cm).

	/** A 100 x 100 mm section. Width is across the beam (Y), depth vertical (Z); depth is squared in W. */
	constexpr double SectionWidthCm = 10.0;
	constexpr double SectionDepthCm = 10.0;

	/** Two segments meeting at x = 0, so the glue line sits at exact midspan. */
	constexpr double SegmentLengthCm = 220.0;
	constexpr double BeamLengthCm = SegmentLengthCm * 2.0;

	/** Bearing centre to bearing centre. */
	constexpr double SpanCm = 400.0;

	/** Length of beam resting on each pier. */
	constexpr double BearingLengthCm = 40.0;

	/*
	 * Piers run past the beam ends (originally to dodge a since-fixed MakeInterface containment bug).
	 * The readings below are anchored to this geometry.
	 */
	constexpr double PierLengthCm = 60.0;
	constexpr double PierHeightCm = 40.0;

	/** A steel plate along the beam. Width matches the beam so it does not contain it on Y. */
	constexpr double BlockLengthCm = 200.0;
	constexpr double BlockWidthCm = SectionWidthCm;

	// Units and published material data, cited, not imported.

	/** 980 cm/s2. MassKg * 980 is already uu force; applying 1 N = 100 uu again is the 100x error. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/**
	 * uu force per MPa per cm2: 1 N = 100 uu, 1 cm2 = 100 mm2, so 10000. Not
	 * DestructionForce::ForceUnitsPerMPaSqCm, so this file fails if that constant is wrong.
	 */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	// C24 softwood, EN 338.

	/*
	 * Bending strength, applied at both extreme fibres (EN 1995-1-1 §6.1.6 checks the whole stress
	 * block; a gravity-loaded beam has no axial force). Mean basis: f_m,k 24 x 1.50 (JCSS lognormal,
	 * CoV 0.25) = 36.
	 */
	constexpr double C24BendingMPa = 36.0;

	// Mean shear: f_v,k 4.0 x 1.50 (JCSS: COV[R_v] = COV[R_m]).
	constexpr double C24ShearMPa = 6.0;

	/** rho_mean 420 kg/m3 = 0.42 g/cm3. Mean, since weight is all density does here. */
	constexpr double C24DensityGramsPerCubicCm = 0.42;

	// S275 structural steel, EN 10025-2.

	/*
	 * Mean static yield (JCSS PMC Part 3 Table A): 285-296 MPa for S275, centre 290. Nominal was 275.
	 * The thick-section reduction (EN 1993-1-1 Table 3.1) changes no verdict and is not applied.
	 */
	constexpr double S275YieldMPa = 290.0;

	/** Von Mises shear yield f_y / sqrt(3), EN 1993-1-1 §6.2.6. */
	const double S275ShearMPa = S275YieldMPa / FMath::Sqrt(3.0);

	/** 7850 kg/m3. */
	constexpr double SteelDensityGramsPerCubicCm = 7.85;

	/** 2400 kg/m3 (EN 1991-1-1 Table A.1). Piers only; they are grounded. */
	constexpr double ConcreteDensityGramsPerCubicCm = 2.4;

	/*
	 * Independent oracle: simply supported beam statics from first principles, derived differently
	 * from the solver's support-graph accumulation.
	 */

	/** W = b d^2 / 6. */
	constexpr double SectionModulusCm3 =
		SectionWidthCm * SectionDepthCm * SectionDepthCm / 6.0;

	/** The beam's cross-section, where the two segments meet. */
	constexpr double SectionAreaSqCm = SectionWidthCm * SectionDepthCm;

	/** Each half-beam takes half the block, centred a quarter block-length from midspan. */
	constexpr double BlockContactOffsetCm = BlockLengthCm / 4.0;

	/** Box mass, kg. cm3 x g/cm3 = grams. */
	double BoxMassKg(double XCm, double YCm, double ZCm, double DensityGramsPerCubicCm)
	{
		return DensityGramsPerCubicCm * XCm * YCm * ZCm / 1000.0;
	}

	/** Both segments together. */
	double BeamMassKg(double DensityGramsPerCubicCm)
	{
		return BoxMassKg(BeamLengthCm, SectionWidthCm, SectionDepthCm, DensityGramsPerCubicCm);
	}

	double BlockMassKg(double BlockHeightCm)
	{
		return BoxMassKg(BlockLengthCm, BlockWidthCm, BlockHeightCm, SteelDensityGramsPerCubicCm);
	}

	/** Reaction at each bearing, uu: half of beam plus block (piers are grounded). */
	double BearingReactionUu(double BlockHeightCm, double BeamDensityGramsPerCubicCm)
	{
		return (BlockMassKg(BlockHeightCm) + BeamMassKg(BeamDensityGramsPerCubicCm))
			* GravityCmPerSecondSquared / 2.0;
	}

	/**
	 * Midspan bending moment, uu.cm, from moments about x = 0 over the left half:
	 * M = R (L/2) - (P/2)(a/4) - (W/2)(l/2).
	 */
	double MidspanMomentUuCm(double BlockHeightCm, double BeamDensityGramsPerCubicCm)
	{
		const double BlockWeightUu = BlockMassKg(BlockHeightCm) * GravityCmPerSecondSquared;
		const double BeamWeightUu =
			BeamMassKg(BeamDensityGramsPerCubicCm) * GravityCmPerSecondSquared;

		return BearingReactionUu(BlockHeightCm, BeamDensityGramsPerCubicCm) * (SpanCm / 2.0)
			- (BlockWeightUu / 2.0) * BlockContactOffsetCm
			- (BeamWeightUu / 2.0) * (SegmentLengthCm / 2.0);
	}

	/** Extreme-fibre bending stress at midspan as a fraction of the member's bending strength. */
	double MidspanBendingUtilisation(
		double BlockHeightCm, double BeamDensityGramsPerCubicCm, double BendingStrengthMPa)
	{
		return MidspanMomentUuCm(BlockHeightCm, BeamDensityGramsPerCubicCm) / SectionModulusCm3
			/ (BendingStrengthMPa * ForceUnitsPerMPaSqCmHere);
	}

	/** Peak shear (1.5 V / A) at a bearing over shear strength, to check bending governs. */
	double BearingShearUtilisation(
		double BlockHeightCm, double BeamDensityGramsPerCubicCm, double ShearStrengthMPa)
	{
		return 1.5 * BearingReactionUu(BlockHeightCm, BeamDensityGramsPerCubicCm) / SectionAreaSqCm
			/ (ShearStrengthMPa * ForceUnitsPerMPaSqCmHere);
	}

	enum class EVerdict : uint8
	{
		/** The member breaks at midspan; beam and load fall, piers stand. */
		PartsAtMidspan,

		/** Nothing anywhere reaches capacity. */
		Stands,
	};

	const TCHAR* VerdictName(EVerdict Verdict)
	{
		return Verdict == EVerdict::PartsAtMidspan ? TEXT("PARTS AT MIDSPAN") : TEXT("STANDS");
	}

	struct FBeamCase
	{
		int32 Number = 0;
		const TCHAR* Title = nullptr;

		/** What this row's matched pair varies. Printed on failure. */
		const TCHAR* Isolates = nullptr;

		const TCHAR* MemberName = nullptr;
		double MemberDensityGramsPerCubicCm = 0.0;

		/** Applied at both extreme fibres. */
		double MemberBendingMPa = 0.0;

		double MemberShearMPa = 0.0;

		/** The only difference between rows 1 and 2. */
		double BlockHeightCm = 0.0;

		EVerdict Verdict = EVerdict::Stands;

		/**
		 * Pieces the model drops today: a characterisation of a known-wrong answer, not an
		 * expectation (mirrors FWallCase::DropsToday). INDEX_NONE means unpinned. When the fix lands,
		 * delete it; never update it to a new wrong number without saying why the answer moved.
		 */
		int32 DropsToday = INDEX_NONE;

		/** Breaking passes today; same rule as DropsToday. Separate so a change in shape shows. */
		int32 PassesToday = INDEX_NONE;
	};

	/**
	 * The member's glue line as a connection profile. mu = 0 because a solid section's shear
	 * strength does not grow with compression (DESIGN.md §3); friction would also flatter the shear
	 * check that proves bending governs.
	 */
	FConnectionStrength MemberStrength(double BendingMPa, double ShearMPa)
	{
		FConnectionStrength Strength;
		Strength.CompressiveStrengthMPa = BendingMPa;
		Strength.ShearCohesionMPa = ShearMPa;
		Strength.TensileStrengthMPa = BendingMPa;
		Strength.FrictionCoefficient = 0.0;

		return Strength;
	}

	TArray<FBeamCase> AllBeamCases()
	{
		/*
		 * No DropsToday pins: since slice 3b/4 the equilibrium LP stands all three bearings. Row 1
		 * stays red on member failure alone (|M| = 0 at midspan), which is evolution step 6.
		 */
		TArray<FBeamCase> Cases;

		/*
		 * Row 1, member failure: a 1884 kg plate on a C24 joist over 4 m. Extreme fibre 83.57 MPa
		 * against the 36 MPa mean, 2.32x capacity.
		 */
		Cases.Add({
			1, TEXT("C24 timber beam, heavy load"), TEXT("member material (vs case 3)"),
			TEXT("C24 timber"), C24DensityGramsPerCubicCm, C24BendingMPa, C24ShearMPa,
			/*BlockHeightCm*/ 120.0, EVerdict::PartsAtMidspan,
			/*DropsToday*/ INDEX_NONE, /*PassesToday*/ INDEX_NONE });

		// Row 2: row 1 with a 157 kg block. 7.41 MPa, 0.206 of capacity; stands.
		Cases.Add({
			2, TEXT("C24 timber beam, light load"), TEXT("load magnitude (vs case 1)"),
			TEXT("C24 timber"), C24DensityGramsPerCubicCm, C24BendingMPa, C24ShearMPa,
			/*BlockHeightCm*/ 10.0, EVerdict::Stands,
			/*DropsToday*/ INDEX_NONE, /*PassesToday*/ INDEX_NONE });

		/*
		 * Row 3: row 1 in S275 steel. 92.22 MPa against yield 290, 0.318; holds. Wood failing where
		 * steel holds is the data-drivenness claim.
		 */
		Cases.Add({
			3, TEXT("S275 steel beam, heavy load"), TEXT("member material (vs case 1)"),
			TEXT("S275 steel"), SteelDensityGramsPerCubicCm, S275YieldMPa, S275ShearMPa,
			/*BlockHeightCm*/ 120.0, EVerdict::Stands,
			/*DropsToday*/ INDEX_NONE, /*PassesToday*/ INDEX_NONE });

		return Cases;
	}

	/** One beam and the handles the assertions need. */
	struct FBeam
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;
		TArray<FString> Names;

		int32 LeftPier = INDEX_NONE;
		int32 RightPier = INDEX_NONE;
		int32 LeftSegment = INDEX_NONE;
		int32 RightSegment = INDEX_NONE;
		int32 Block = INDEX_NONE;

		int32 MidspanJoint = INDEX_NONE;
		int32 LeftBearingJoint = INDEX_NONE;
		int32 RightBearingJoint = INDEX_NONE;
	};

	/** The joint naming these two pieces, either way round, or INDEX_NONE. */
	int32 JointBetween(const FBeam& Beam, int32 First, int32 Second)
	{
		for (int32 Index = 0; Index < Beam.Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Beam.Structure.GetConnection(Index);

			if ((Joint.PieceA == First && Joint.PieceB == Second)
				|| (Joint.PieceA == Second && Joint.PieceB == First))
			{
				return Index;
			}
		}

		return INDEX_NONE;
	}

	/*
	 * Area two boxes share, by interval intersection. A consistency check on the fixture; it matches
	 * MakeInterface's own arithmetic, so it is not an independent oracle (see Layout.InterfaceFuzz).
	 */
	double SharedFaceAreaSqCm(const FPieceBox& A, const FPieceBox& B, int32 SeparationAxis)
	{
		double AreaSqCm = 1.0;

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Axis == SeparationAxis)
			{
				continue;
			}

			const double LowCm = FMath::Max(A.CentreCm[Axis] - A.ExtentCm[Axis],
				B.CentreCm[Axis] - B.ExtentCm[Axis]);
			const double HighCm = FMath::Min(A.CentreCm[Axis] + A.ExtentCm[Axis],
				B.CentreCm[Axis] + B.ExtentCm[Axis]);

			AreaSqCm *= HighCm - LowCm;
		}

		return AreaSqCm;
	}

	/** Lay one row's beam. Joint thickness is zero: every joint is direct contact. */
	void LayBeam(const FBeamCase& Case, FBeam& OutBeam)
	{
		const double BeamBottomZCm = PierHeightCm;
		const double BeamCentreZCm = BeamBottomZCm + SectionDepthCm / 2.0;
		const double BeamTopZCm = BeamBottomZCm + SectionDepthCm;

		// Each bearing patch is centred SpanCm/2 from midspan. See PierLengthCm.
		const double BeamEndCm = BeamLengthCm / 2.0;
		const double PierInnerCm = BeamEndCm - BearingLengthCm;
		const double PierCentreCm = PierInnerCm + PierLengthCm / 2.0;

		const auto AddBox = [&OutBeam](
			const FString& Name, const FPieceBox& Box, double DensityGramsPerCubicCm, bool bGrounded)
		{
			// Box centre = centre of mass. Without it the bearings would read a centred load.
			const double MassKg = BoxMassKg(
				Box.ExtentCm.X * 2.0, Box.ExtentCm.Y * 2.0, Box.ExtentCm.Z * 2.0,
				DensityGramsPerCubicCm);

			const int32 Handle = OutBeam.Structure.AddPiece(MassKg, bGrounded, Box.CentreCm);

			OutBeam.Boxes.Add(Box);
			OutBeam.Names.Add(Name);

			return Handle;
		};

		FPieceBox PierBox;
		PierBox.ExtentCm = FVector(PierLengthCm, SectionWidthCm, PierHeightCm) * 0.5;

		PierBox.CentreCm = FVector(-PierCentreCm, 0.0, PierHeightCm / 2.0);
		OutBeam.LeftPier = AddBox(
			TEXT("left pier"), PierBox, ConcreteDensityGramsPerCubicCm, /*bGrounded*/ true);

		PierBox.CentreCm = FVector(PierCentreCm, 0.0, PierHeightCm / 2.0);
		OutBeam.RightPier = AddBox(
			TEXT("right pier"), PierBox, ConcreteDensityGramsPerCubicCm, /*bGrounded*/ true);

		FPieceBox SegmentBox;
		SegmentBox.ExtentCm = FVector(SegmentLengthCm, SectionWidthCm, SectionDepthCm) * 0.5;

		SegmentBox.CentreCm = FVector(-SegmentLengthCm / 2.0, 0.0, BeamCentreZCm);
		OutBeam.LeftSegment = AddBox(
			TEXT("left half-beam"), SegmentBox, Case.MemberDensityGramsPerCubicCm, false);

		SegmentBox.CentreCm = FVector(SegmentLengthCm / 2.0, 0.0, BeamCentreZCm);
		OutBeam.RightSegment = AddBox(
			TEXT("right half-beam"), SegmentBox, Case.MemberDensityGramsPerCubicCm, false);

		FPieceBox BlockBox;
		BlockBox.ExtentCm = FVector(BlockLengthCm, BlockWidthCm, Case.BlockHeightCm) * 0.5;
		BlockBox.CentreCm = FVector(0.0, 0.0, BeamTopZCm + Case.BlockHeightCm / 2.0);
		OutBeam.Block = AddBox(TEXT("block"), BlockBox, SteelDensityGramsPerCubicCm, false);

		/*
		 * Offer every pair; MakeInterface rejects non-faces. Only the segment-to-segment joint gets
		 * the member's strengths; the rest are dry.
		 */
		const FConnectionStrength Member = MemberStrength(Case.MemberBendingMPa, Case.MemberShearMPa);

		for (int32 First = 0; First < OutBeam.Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < OutBeam.Boxes.Num(); ++Second)
			{
				const bool bIsTheMember =
					(First == OutBeam.LeftSegment && Second == OutBeam.RightSegment)
					|| (First == OutBeam.RightSegment && Second == OutBeam.LeftSegment);

				FConnection Joint;

				if (MakeInterface(
						First, OutBeam.Boxes[First], Second, OutBeam.Boxes[Second],
						/*JointThicknessCm*/ 0.0, bIsTheMember ? Member : DryStone, Joint))
				{
					OutBeam.Structure.AddConnection(Joint);
				}
			}
		}

		OutBeam.MidspanJoint = JointBetween(OutBeam, OutBeam.LeftSegment, OutBeam.RightSegment);
		OutBeam.LeftBearingJoint = JointBetween(OutBeam, OutBeam.LeftPier, OutBeam.LeftSegment);
		OutBeam.RightBearingJoint = JointBetween(OutBeam, OutBeam.RightPier, OutBeam.RightSegment);
	}

	/** Live pieces with no path to the earth. Stranded counts as fallen. */
	TArray<int32> FallenPieces(const FBeam& Beam)
	{
		TArray<int32> Fallen;

		for (int32 Piece = 0; Piece < Beam.Structure.NumPieces(); ++Piece)
		{
			if (Beam.Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			const EPieceSupport Support = Beam.Structure.GetPieceSupport(Piece);

			if (Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported)
			{
				Fallen.Add(Piece);
			}
		}

		return Fallen;
	}

	/** Live pieces the solver could not route. A precondition, not a verdict. */
	int32 StrandedCount(const FBeam& Beam)
	{
		int32 Stranded = 0;

		for (int32 Piece = 0; Piece < Beam.Structure.NumPieces(); ++Piece)
		{
			if (!Beam.Structure.IsPieceRemoved(Piece)
				&& Beam.Structure.GetPieceSupport(Piece) == EPieceSupport::Stranded)
			{
				++Stranded;
			}
		}

		return Stranded;
	}

	FString DescribePieces(const FBeam& Beam, const TArray<int32>& Pieces)
	{
		if (Pieces.Num() == 0)
		{
			return TEXT("{}");
		}

		FString Line = TEXT("{");

		for (int32 Index = 0; Index < Pieces.Num(); ++Index)
		{
			Line += (Index == 0 ? TEXT("") : TEXT(", "));
			Line += Beam.Names[Pieces[Index]];
		}

		return Line + TEXT("}");
	}

	/** One row's readings as built, then after the cascade. */
	struct FBeamResult
	{
		bool bLaid = false;

		/** From the non-destructive solve, before anything breaks. */
		double LeftBearingUu = 0.0;
		double RightBearingUu = 0.0;
		double MidspanForceUu = 0.0;
		double MidspanMomentUuCm = 0.0;
		double MidspanUtilisation = 0.0;
		double WorstUtilisation = 0.0;
		int32 WorstJoint = INDEX_NONE;

		/** Passes that broke at least one joint. */
		int32 Passes = 0;

		TArray<int32> Fallen;
		int32 Stranded = 0;
		bool bMidspanGave = false;
	};

	/** Lay, read with SolveLoads (non-destructive), then run SolveAndBreak for the verdict. */
	void RunBeamCase(
		FAutomationTestBase& Test, const FBeamCase& Case, FBeam& OutBeam, FBeamResult& OutResult)
	{
		LayBeam(Case, OutBeam);

		const FString Where =
			FString::Printf(TEXT("case %d (%s)"), Case.Number, Case.Title);

		if (OutBeam.MidspanJoint == INDEX_NONE
			|| OutBeam.LeftBearingJoint == INDEX_NONE
			|| OutBeam.RightBearingJoint == INDEX_NONE)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: FIXTURE the producer did not emit the midspan glue line and both ")
				TEXT("bearings; it emitted %d joint(s) in total"),
				*Where, OutBeam.Structure.NumConnections()));

			return;
		}

		OutResult.bLaid = true;

		OutBeam.Structure.SolveLoads();

		OutResult.LeftBearingUu =
			FMath::Abs(OutBeam.Structure.GetConnectionForce(OutBeam.LeftBearingJoint).Z);
		OutResult.RightBearingUu =
			FMath::Abs(OutBeam.Structure.GetConnectionForce(OutBeam.RightBearingJoint).Z);
		OutResult.MidspanForceUu =
			OutBeam.Structure.GetConnectionForce(OutBeam.MidspanJoint).Size();
		OutResult.MidspanMomentUuCm =
			OutBeam.Structure.GetConnectionMoment(OutBeam.MidspanJoint).Size();
		OutResult.MidspanUtilisation =
			OutBeam.Structure.GetConnectionUtilisation(OutBeam.MidspanJoint);

		for (int32 Index = 0; Index < OutBeam.Structure.NumConnections(); ++Index)
		{
			const double Utilisation = OutBeam.Structure.GetConnectionUtilisation(Index);

			if (Utilisation > OutResult.WorstUtilisation)
			{
				OutResult.WorstUtilisation = Utilisation;
				OutResult.WorstJoint = Index;
			}
		}

		OutResult.Passes = OutBeam.Structure.SolveAndBreak();
		OutResult.Fallen = FallenPieces(OutBeam);
		OutResult.Stranded = StrandedCount(OutBeam);
		OutResult.bMidspanGave =
			OutBeam.Structure.GetConnection(OutBeam.MidspanJoint).HasGiven();
	}

	/** Prints every reading, pass or fail. */
	void ReportBeamCase(
		FAutomationTestBase& Test, const FBeamCase& Case, const FBeam& Beam, const FBeamResult& Result)
	{
		if (!Result.bLaid)
		{
			return;
		}

		const double DerivedMomentUuCm =
			MidspanMomentUuCm(Case.BlockHeightCm, Case.MemberDensityGramsPerCubicCm);
		const double DerivedBending = MidspanBendingUtilisation(
			Case.BlockHeightCm, Case.MemberDensityGramsPerCubicCm, Case.MemberBendingMPa);
		const double DerivedShear = BearingShearUtilisation(
			Case.BlockHeightCm, Case.MemberDensityGramsPerCubicCm, Case.MemberShearMPa);

		Test.AddInfo(FString::Printf(
			TEXT("case %d (%s) [%s]: %s. block %.4g kg, beam %.4g kg. ")
			TEXT("DERIVED midspan M %.10g uu.cm, bending %.10g of f_m, shear %.10g of f_v. ")
			TEXT("SOLVER bearings %.10g + %.10g = %.10g uu (total weight %.10g uu); ")
			TEXT("midspan |F| %.10g uu, |M| %.10g uu.cm, utilisation %.10g; ")
			TEXT("worst joint %d at %.10g; passes %d; fallen %s; stranded %d"),
			Case.Number, Case.Title, Case.MemberName, VerdictName(Case.Verdict),
			BlockMassKg(Case.BlockHeightCm), BeamMassKg(Case.MemberDensityGramsPerCubicCm),
			DerivedMomentUuCm, DerivedBending, DerivedShear,
			Result.LeftBearingUu, Result.RightBearingUu,
			Result.LeftBearingUu + Result.RightBearingUu,
			(BlockMassKg(Case.BlockHeightCm) + BeamMassKg(Case.MemberDensityGramsPerCubicCm))
				* GravityCmPerSecondSquared,
			Result.MidspanForceUu, Result.MidspanMomentUuCm, Result.MidspanUtilisation,
			Result.WorstJoint, Result.WorstUtilisation, Result.Passes,
			*DescribePieces(Beam, Result.Fallen), Result.Stranded));
	}

	/** Preconditions without which a row's verdict is about the fixture, not the physics. */
	void CheckFixture(
		FAutomationTestBase& Test, const FBeamCase& Case, const FBeam& Beam, const FBeamResult& Result)
	{
		const FString Where = FString::Printf(TEXT("case %d (%s)"), Case.Number, Case.Title);

		Test.TestEqual(
			*FString::Printf(
				TEXT("%s: FIXTURE the producer must emit exactly five faces — one glue line, two ")
				TEXT("bearings, two contacts under the block"),
				*Where),
			Beam.Structure.NumConnections(), 5);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: FIXTURE every piece and every joint must know where it is, or there are ")
				TEXT("no moments anywhere and the bearings read a centred load"),
				*Where),
			Beam.Structure.HasCompleteGeometry());

		// Every emitted area against SharedFaceAreaSqCm.
		for (int32 Index = 0; Index < Beam.Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Beam.Structure.GetConnection(Index);

			int32 SeparationAxis = INDEX_NONE;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if (FMath::Abs(Joint.InterfaceNormal[Axis]) > 0.5)
				{
					SeparationAxis = Axis;
				}
			}

			const double TrueAreaSqCm = SharedFaceAreaSqCm(
				Beam.Boxes[Joint.PieceA], Beam.Boxes[Joint.PieceB], SeparationAxis);

			Test.TestEqual(
				*FString::Printf(
					TEXT("%s: FIXTURE joint %d (%s-%s) must be the face those two boxes really ")
					TEXT("share"),
					*Where, Index, *Beam.Names[Joint.PieceA], *Beam.Names[Joint.PieceB]),
				Joint.InterfaceAreaSqCm, TrueAreaSqCm);
		}

		Test.TestEqual(
			*FString::Printf(
				TEXT("%s: FIXTURE no piece may be Stranded — a verdict decided by the solver ")
				TEXT("declining to divide load round a loop is not a verdict about a beam"),
				*Where),
			Result.Stranded, 0);

		// Bending must govern over shear (ratios 9.96, 9.58, 32.5).
		const double DerivedBending = MidspanBendingUtilisation(
			Case.BlockHeightCm, Case.MemberDensityGramsPerCubicCm, Case.MemberBendingMPa);
		const double DerivedShear = BearingShearUtilisation(
			Case.BlockHeightCm, Case.MemberDensityGramsPerCubicCm, Case.MemberShearMPa);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: FIXTURE midspan bending (%.10g) must govern the member by a clear ")
				TEXT("margin over shear at the bearing (%.10g)"),
				*Where, DerivedBending, DerivedShear),
			DerivedBending > DerivedShear * 5.0);
	}
}

/**
 * Three rows with real-world verdicts. Case 1 vs 2 varies only load; 1 vs 3 only material.
 *
 * Row 1 checks both mechanism (the glue line gave, not a slide off the bearings) and outcome (beam
 * and load fall, piers stand; DESIGN.md §4). The outcome also needs a half-beam to pivot off its
 * pier, which needs global equilibrium, so row 1 is anchored on evolution step 6 as a whole.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBeamAcceptanceCatalogueTest,
	"DestructionGame.Acceptance.Beam.Catalogue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBeamAcceptanceCatalogueTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace BeamAcceptanceTestSupport;

	// Dry contact is DryStone; a retune should fail here rather than silently move every case.
	TestEqual(TEXT("FIXTURE: dry contact has no cohesion at all"),
		DryStone.ShearCohesionMPa, 0.0);

	TestEqual(TEXT("FIXTURE: dry contact carries shear by friction alone, at mu = 0.7"),
		DryStone.FrictionCoefficient, 0.7);

	TestEqual(TEXT("FIXTURE: the section modulus of a 100 x 100 mm section is b d^2 / 6"),
		SectionModulusCm3, 10.0 * 10.0 * 10.0 / 6.0);

	const TArray<FBeamCase> Cases = AllBeamCases();

	TestEqual(TEXT("FIXTURE: the catalogue is three cases"), Cases.Num(), 3);

	for (const FBeamCase& Case : Cases)
	{
		FBeam Beam;
		FBeamResult Result;

		RunBeamCase(*this, Case, Beam, Result);
		ReportBeamCase(*this, Case, Beam, Result);

		if (!Result.bLaid)
		{
			continue;
		}

		CheckFixture(*this, Case, Beam, Result);

		const FString Where = FString::Printf(
			TEXT("case %d (%s) [isolates %s]"), Case.Number, Case.Title, Case.Isolates);

		// The two bearings carry the full weight, on every row.
		const double TotalWeightUu =
			(BlockMassKg(Case.BlockHeightCm) + BeamMassKg(Case.MemberDensityGramsPerCubicCm))
			* GravityCmPerSecondSquared;

		TestNearlyEqual(
			*FString::Printf(
				TEXT("%s: the two bearings must carry the whole of the beam and its load; they ")
				TEXT("carry %.10g + %.10g against a total weight of %.10g uu"),
				*Where, Result.LeftBearingUu, Result.RightBearingUu, TotalWeightUu),
			Result.LeftBearingUu + Result.RightBearingUu, TotalWeightUu,
			TotalWeightUu * 1.0e-9);

		// Known-wrong-answer pin, if set. See FBeamCase::DropsToday.
		if (Case.DropsToday != INDEX_NONE)
		{
			TestEqual(
				*FString::Printf(
					TEXT("%s: CHARACTERISATION of a KNOWN RED — since the one-cell thrust gate ")
					TEXT("(08abcfd, 2026-08-09) these DRY bearings lose the kern cap and read Max(), ")
					TEXT("so the model drops %d piece(s) here today regardless of the catalogue's %s ")
					TEXT("verdict. It dropped %d: %s. ACCEPTED AS A KNOWN COST until evolution step 4 ")
					TEXT("(DESIGN.md §7's path; the beam user ruling, DESIGN.md §8, decided ")
					TEXT("2026-08-11) — global equilibrium is what fixes the bearings honestly, not ")
					TEXT("this fixture. If a slice just fixed this row, DELETE its DropsToday and ")
					TEXT("PassesToday in the same edit; if nothing here was meant to change, the ")
					TEXT("model's answer has moved and something else moved it."),
					*Where, Case.DropsToday, VerdictName(Case.Verdict),
					Result.Fallen.Num(), *DescribePieces(Beam, Result.Fallen)),
				Result.Fallen.Num(), Case.DropsToday);

			TestEqual(
				*FString::Printf(
					TEXT("%s: CHARACTERISATION of a KNOWN RED — the cascade runs %d breaking pass(es) ")
					TEXT("here today; see DropsToday just above for the mechanism and the ")
					TEXT("delete-when-fixed instruction, which covers this pin too."),
					*Where, Case.PassesToday),
				Result.Passes, Case.PassesToday);
		}

		if (Case.Verdict == EVerdict::Stands)
		{
			// Both: nothing fell and no joint gave.
			TestEqual(
				*FString::Printf(
					TEXT("%s: STANDS means nothing lost the earth; %s did"),
					*Where, *DescribePieces(Beam, Result.Fallen)),
				Result.Fallen.Num(), 0);

			TestEqual(
				*FString::Printf(
					TEXT("%s: STANDS means no joint gave; the cascade ran %d breaking pass(es)"),
					*Where, Result.Passes),
				Result.Passes, 0);

			continue;
		}

		// Mechanism: the midspan glue line, the member's critical section, must give.
		TestTrue(
			*FString::Printf(
				TEXT("%s: the member must break at midspan; the glue line %s given, and it read ")
				TEXT("|M| = %.10g uu.cm against the %.10g uu.cm a beam that shape carries there"),
				*Where, Result.bMidspanGave ? TEXT("has") : TEXT("has NOT"),
				Result.MidspanMomentUuCm,
				MidspanMomentUuCm(Case.BlockHeightCm, Case.MemberDensityGramsPerCubicCm)),
			Result.bMidspanGave);

		// Outcome, two-sided: beam and load fall, piers stand.
		const TArray<int32> MustFall{ Beam.LeftSegment, Beam.RightSegment, Beam.Block };
		const TArray<int32> MustStand{ Beam.LeftPier, Beam.RightPier };

		TArray<int32> WronglyStanding;
		for (const int32 Piece : MustFall)
		{
			if (!Result.Fallen.Contains(Piece))
			{
				WronglyStanding.Add(Piece);
			}
		}

		TestEqual(
			*FString::Printf(
				TEXT("%s: a broken beam takes itself and its load down; %s kept the earth"),
				*Where, *DescribePieces(Beam, WronglyStanding)),
			WronglyStanding.Num(), 0);

		TArray<int32> WronglyFallen;
		for (const int32 Piece : MustStand)
		{
			if (Result.Fallen.Contains(Piece))
			{
				WronglyFallen.Add(Piece);
			}
		}

		TestEqual(
			*FString::Printf(
				TEXT("%s: the piers are not what failed; %s lost the earth"),
				*Where, *DescribePieces(Beam, WronglyFallen)),
			WronglyFallen.Num(), 0);
	}

	return true;
}

/**
 * The midspan section must carry the statics bending moment (PROJECT_REVIEW.md §2 item 3). Today
 * it reads exactly zero, since each half-beam seats on its own pier.
 *
 * The 10% band allows a solver to place the load resultant slightly differently across the 200 cm
 * contact; it does not admit zero. All three rows, since the gap is in routing, not material.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBeamAcceptanceMidspanMomentTest,
	"DestructionGame.Acceptance.Beam.MidspanCarriesTheMembersBendingMoment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBeamAcceptanceMidspanMomentTest::RunTest(const FString& Parameters)
{
	using namespace BeamAcceptanceTestSupport;

	for (const FBeamCase& Case : AllBeamCases())
	{
		FBeam Beam;
		FBeamResult Result;

		RunBeamCase(*this, Case, Beam, Result);
		ReportBeamCase(*this, Case, Beam, Result);

		if (!Result.bLaid)
		{
			continue;
		}

		const double ExpectedUuCm =
			MidspanMomentUuCm(Case.BlockHeightCm, Case.MemberDensityGramsPerCubicCm);

		TestTrue(
			*FString::Printf(
				TEXT("case %d (%s): the midspan section carries the beam's bending moment; it ")
				TEXT("reads %.10g uu.cm against %.10g, a ratio of %.10g"),
				Case.Number, Case.Title, Result.MidspanMomentUuCm, ExpectedUuCm,
				ExpectedUuCm == 0.0 ? 0.0 : Result.MidspanMomentUuCm / ExpectedUuCm),
			FMath::Abs(Result.MidspanMomentUuCm - ExpectedUuCm) <= ExpectedUuCm * 0.1);
	}

	return true;
}

/**
 * The member material decides the outcome: same geometry and load, timber fails and steel holds
 * (DESIGN.md §4's data-drivenness check). A model keyed only on load magnitude cannot pass this.
 *
 * Green since first-crack (2026-08-28) via the glue line cracking (C24 lambda* 0.883 falls, S275
 * 6.10 holds), not true member bending; that is still evolution step 6
 * (Beam.MidspanCarriesTheMembersBendingMoment).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBeamAcceptanceMemberMaterialTest,
	"DestructionGame.Acceptance.Beam.TheMemberMaterialDecidesTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBeamAcceptanceMemberMaterialTest::RunTest(const FString& Parameters)
{
	using namespace BeamAcceptanceTestSupport;

	const TArray<FBeamCase> Cases = AllBeamCases();

	// Not `Timber`: that shadows DestructionProfiles::Timber (C4459, warning-as-error).
	const FBeamCase& TimberCase = Cases[0];
	const FBeamCase& Steel = Cases[2];

	TestEqual(TEXT("FIXTURE: the pair carries the same block, so only the member differs"),
		TimberCase.BlockHeightCm, Steel.BlockHeightCm);

	TestTrue(TEXT("FIXTURE: the pair really is two different member materials"),
		TimberCase.MemberBendingMPa != Steel.MemberBendingMPa);

	FBeam TimberBeam;
	FBeamResult TimberResult;
	RunBeamCase(*this, TimberCase, TimberBeam, TimberResult);
	ReportBeamCase(*this, TimberCase, TimberBeam, TimberResult);

	FBeam SteelBeam;
	FBeamResult SteelResult;
	RunBeamCase(*this, Steel, SteelBeam, SteelResult);
	ReportBeamCase(*this, Steel, SteelBeam, SteelResult);

	if (!TimberResult.bLaid || !SteelResult.bLaid)
	{
		return false;
	}

	TestTrue(
		*FString::Printf(
			TEXT("the same load on the same beam must part C24 (%.10g of f_m,k) and not S275 ")
			TEXT("(%.10g of f_y); the model gave the timber %d fallen piece(s) and the steel %d"),
			MidspanBendingUtilisation(
				TimberCase.BlockHeightCm, TimberCase.MemberDensityGramsPerCubicCm, TimberCase.MemberBendingMPa),
			MidspanBendingUtilisation(
				Steel.BlockHeightCm, Steel.MemberDensityGramsPerCubicCm, Steel.MemberBendingMPa),
			TimberResult.Fallen.Num(), SteelResult.Fallen.Num()),
		TimberResult.Fallen.Num() > SteelResult.Fallen.Num());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

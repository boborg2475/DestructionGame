// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/PieceMenu.h"

#include "Core/Connection.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

/*
 * Presenter prefix on every name: an anonymous namespace is private to a translation unit, and
 * a unity build merges files, so two file-local names can collide (see Structure.cpp).
 */
namespace
{
	/** A joint's tier in player-facing words. None gets its own word so it never reads as a real tier. */
	const TCHAR* PresenterWordForJointRole(EJointRole Role)
	{
		switch (Role)
		{
		case EJointRole::BedBeneath: return TEXT("bed below");
		case EJointRole::BedAbove:   return TEXT("bed above");
		case EJointRole::Head:       return TEXT("head");
		case EJointRole::None:       return TEXT("no tier");
		}

		return TEXT("no tier");
	}

	/**
	 * The joint's connection profile, named by its library row in lower case (it sits mid-line).
	 * Matched by value because FConnection stores a copy of the profile. A strength matching no
	 * shipped row reads "custom" rather than naming the nearest row.
	 */
	FString PresenterWordForJointProfile(const FStructure& Structure, int32 ConnectionIndex)
	{
		if (ConnectionIndex >= 0 && ConnectionIndex < Structure.NumConnections())
		{
			const DestructionProfiles::FNamedConnectionProfile* const Row =
				DestructionProfiles::FindConnectionProfileRow(
					Structure.GetConnection(ConnectionIndex).Strength);

			if (Row != nullptr && Row->Name != nullptr)
			{
				return FString(Row->Name).ToLower();
			}
		}

		return FString(TEXT("custom"));
	}

	/** The panel heading. Kept here, not in the widget, so all wording stays testable. */
	const TCHAR* const PresenterPanelHeader = TEXT("Selection");

	/** What the readout region says when no brick is inspected, so it does not read as a failure. */
	const TCHAR* const PresenterInspectedHint = TEXT("Hover a brick in the list to see its joints");

	/**
	 * The support bucket a brick's word and colour are read off. Order matters: "not a piece" is
	 * checked before "not solved" (a removed brick would otherwise read NotSolved, like a live
	 * one), and "not solved" before the enumerator, because EPieceSupport::Falling is also the
	 * default for an unanswered piece. Shared by the entry rows and the readout so they agree.
	 */
	EPieceSupportBand PresenterSupportBand(const FPieceInspection& Inspection)
	{
		if (!Inspection.bIsPiece)
		{
			return EPieceSupportBand::NotAPiece;
		}

		if (!Inspection.bHasSupportAnswer)
		{
			return EPieceSupportBand::NotSolved;
		}

		switch (Inspection.Support)
		{
		case EPieceSupport::Grounded:  return EPieceSupportBand::Grounded;
		case EPieceSupport::Supported: return EPieceSupportBand::Supported;
		case EPieceSupport::Stranded:  return EPieceSupportBand::Stranded;
		case EPieceSupport::Falling:   return EPieceSupportBand::Falling;
		}

		return EPieceSupportBand::Falling;
	}

	/**
	 * The bucket in player-facing words. Derived from the bucket so word and colour cannot
	 * disagree; every bucket gets a distinct sentence.
	 */
	FString PresenterWordForSupportBand(EPieceSupportBand Band)
	{
		switch (Band)
		{
		case EPieceSupportBand::NotAPiece: return FString(TEXT("not in this wall"));
		case EPieceSupportBand::NotSolved: return FString(TEXT("not solved yet"));
		case EPieceSupportBand::Grounded:  return FString(TEXT("grounded"));
		case EPieceSupportBand::Supported: return FString(TEXT("supported"));
		case EPieceSupportBand::Stranded:  return FString(TEXT("stranded"));
		case EPieceSupportBand::Falling:   return FString(TEXT("falling"));
		}

		return FString(TEXT("falling"));
	}

	/** A brick's joint count in words, with singular/plural and a sentence for zero. */
	FString PresenterWordForJointCount(int32 JointCount)
	{
		if (JointCount == 0)
		{
			return FString(TEXT("No joints"));
		}

		if (JointCount == 1)
		{
			return FString(TEXT("1 joint"));
		}

		return FString::Printf(TEXT("%d joints"), JointCount);
	}

	/** How many bricks a row acts on ("1 brick", "11 bricks"). A row always has at least one. */
	FString PresenterWordForTargetCount(int32 BrickCount)
	{
		return BrickCount == 1
			? FString(TEXT("1 brick"))
			: FString::Printf(TEXT("%d bricks"), BrickCount);
	}

	/**
	 * A piece's material, named by its library row. Matched by address, since PlacePiece stores a
	 * pointer to the shipped constant. Null is common (RunningBond sets only a density) and reads
	 * "Unknown material".
	 */
	FString PresenterWordForMaterial(const DestructionProfiles::FMaterialProfile* Material)
	{
		if (Material != nullptr)
		{
			for (const DestructionProfiles::FNamedMaterialProfile& Row :
				DestructionProfiles::AllMaterialProfiles())
			{
				if (&Row.Profile == Material && Row.Name != nullptr)
				{
					return FString(Row.Name);
				}
			}
		}

		return FString(TEXT("Unknown material"));
	}

	/**
	 * A length in cm, at most two decimals (the grid's resolution), trailing zeros trimmed. "%g"
	 * is avoided because it switches to scientific notation. The trim stops at the point, so
	 * "1000.00" becomes "1000", not "1", and a NaN passes through as a word.
	 */
	FString PresenterCentimetreText(double LengthCm)
	{
		FString Text = FString::Printf(TEXT("%.2f"), LengthCm);

		while (Text.EndsWith(TEXT("0"), ESearchCase::CaseSensitive))
		{
			Text.LeftChopInline(1);
		}

		if (Text.EndsWith(TEXT("."), ESearchCase::CaseSensitive))
		{
			Text.LeftChopInline(1);
		}

		return Text;
	}

	/**
	 * The inspected brick's material, size and mass as one line. Size is twice
	 * FPieceBox::ExtentCm, which is a half size. Mass is the piece's own MassKg (what the solver
	 * routes), not recomputed from the box. No force here, so no conversion boundary.
	 */
	FString PresenterIdentityLine(const FStructureBinding& Binding, int32 PieceIndex)
	{
		const FVector FullSizeCm = 2.0 * Binding.GetBinding(PieceIndex).Box.ExtentCm;
		const FStructurePiece& Piece = Binding.GetStructure().GetPiece(PieceIndex);

		return FString::Printf(
			TEXT("%s · %s × %s × %s cm · %.1f kg"),
			*PresenterWordForMaterial(Piece.Material),
			*PresenterCentimetreText(FullSizeCm.X),
			*PresenterCentimetreText(FullSizeCm.Y),
			*PresenterCentimetreText(FullSizeCm.Z),
			Piece.MassKg);
	}

	/**
	 * How far apart two centres may sit in Z and still be one course. 0.5 cm against a 7.5 cm
	 * course pitch: kept tight because merging two courses fails silently, while splitting one
	 * is obvious. Absolute, so pieces under about 1 cm tall would band together; a relative
	 * tolerance would make "same course" non-transitive.
	 */
	constexpr double PresenterCourseToleranceCm = 0.5;

	/** One piece on its way to a position: where it is, and which course it landed in. */
	struct FPresenterPlacedPiece
	{
		int32 Handle = INDEX_NONE;
		FVector CentreCm = FVector::ZeroVector;
		int32 Course = INDEX_NONE;
	};

	/**
	 * Whether a centre can be banded and sorted. A NaN breaks the sort's strict weak ordering
	 * (undefined behaviour) and an infinity gets an unstable course, so both are excluded.
	 */
	bool PresenterHasUsableCentre(const FVector& CentreCm)
	{
		return FMath::IsFinite(CentreCm.X)
			&& FMath::IsFinite(CentreCm.Y)
			&& FMath::IsFinite(CentreCm.Z);
	}

	/**
	 * Order along a course: X, then Y (a two-leaf wall has bricks at the same X), then handle,
	 * giving a total order so no two pieces share an ordinal.
	 */
	bool PresenterIsEarlierAlongCourse(
		const FPresenterPlacedPiece& A,
		const FPresenterPlacedPiece& B)
	{
		if (A.CentreCm.X != B.CentreCm.X)
		{
			return A.CentreCm.X < B.CentreCm.X;
		}

		if (A.CentreCm.Y != B.CentreCm.Y)
		{
			return A.CentreCm.Y < B.CentreCm.Y;
		}

		return A.Handle < B.Handle;
	}

	/**
	 * A position label ("course 2 · #1") for every piece, indexed by handle. Done here because
	 * FStructure has no positions; the binding holds the boxes. An unplaceable handle gets an
	 * empty string and the caller falls back to the ref label.
	 */
	TArray<FString> PresenterPositionLabels(const FStructureBinding& Binding)
	{
		TArray<FString> Labels;
		Labels.SetNum(Binding.NumPieces());

		TArray<FPresenterPlacedPiece> Placed;
		Placed.Reserve(Binding.NumPieces());

		// Removed pieces stay placed, so pulling a brick does not renumber its course.
		for (int32 Handle = 0; Handle < Binding.NumPieces(); ++Handle)
		{
			const FVector& CentreCm = Binding.GetBinding(Handle).Box.CentreCm;

			if (!PresenterHasUsableCentre(CentreCm))
			{
				continue;
			}

			FPresenterPlacedPiece& Piece = Placed.AddDefaulted_GetRef();
			Piece.Handle = Handle;
			Piece.CentreCm = CentreCm;
		}

		Placed.Sort([](const FPresenterPlacedPiece& A, const FPresenterPlacedPiece& B)
			{
				return A.CentreCm.Z != B.CentreCm.Z
					? A.CentreCm.Z < B.CentreCm.Z
					: PresenterIsEarlierAlongCourse(A, B);
			});

		/*
		 * Band against the course's floor, not the previous piece: comparing neighbours chains,
		 * so a slow ramp of pieces would merge into one course.
		 */
		int32 Course = 0;
		double CourseFloorZCm = 0.0;

		for (FPresenterPlacedPiece& Piece : Placed)
		{
			const bool bJoinsCourse = Course > 0
				&& (Piece.CentreCm.Z - CourseFloorZCm) <= PresenterCourseToleranceCm;

			if (!bJoinsCourse)
			{
				++Course;
				CourseFloorZCm = Piece.CentreCm.Z;
			}

			Piece.Course = Course;
		}

		Placed.Sort([](const FPresenterPlacedPiece& A, const FPresenterPlacedPiece& B)
			{
				return A.Course != B.Course
					? A.Course < B.Course
					: PresenterIsEarlierAlongCourse(A, B);
			});

		// Both numbers count from one, as a person would.
		int32 PositionInCourse = 0;
		int32 CurrentCourse = INDEX_NONE;

		for (const FPresenterPlacedPiece& Piece : Placed)
		{
			PositionInCourse = Piece.Course == CurrentCourse ? PositionInCourse + 1 : 1;
			CurrentCourse = Piece.Course;

			Labels[Piece.Handle] =
				FString::Printf(TEXT("course %d · #%d"), Piece.Course, PositionInCourse);
		}

		return Labels;
	}

	/**
	 * Newtons per kilonewton. Display only, not a conversion boundary: the uu-to-N boundary
	 * (DestructionPresenter::ForceUnitsPerNewton) has already been applied.
	 */
	constexpr double PresenterNewtonsPerKilonewton = 1000.0;

	/** A joint force in N, or in kN from 1000 N up (inclusive). */
	FString PresenterForceText(double ForceN)
	{
		return ForceN >= PresenterNewtonsPerKilonewton
			? FString::Printf(TEXT("%.1f kN"), ForceN / PresenterNewtonsPerKilonewton)
			: FString::Printf(TEXT("%.1f N"), ForceN);
	}

	/**
	 * A trailing bending clause, empty when the moment is exactly zero (most joints; see
	 * FStructure::GetConnectionMoment). A NaN is not equal to zero, so it still prints. Already
	 * in N·cm; N·cm rather than N·m keeps a useful digit.
	 */
	FString PresenterBendingText(double MomentNCm)
	{
		return MomentNCm == 0.0
			? FString()
			: FString::Printf(TEXT("  %.1f N·cm bending"), MomentNCm);
	}

	/** What a joint's utilisation reads when it is carrying exactly all it can. */
	constexpr double PresenterFullLoadPercent = 100.0;

	/** At or above this many times its load, a joint's margin loses its decimal. */
	constexpr double PresenterWholeMarginAtTimes = 100.0;

	/**
	 * How many times its load the joint could take ("2041× margin"), the reciprocal of the
	 * percentage on the row. Given, at-or-past-limit and unloaded joints get words instead,
	 * since the reciprocal would mislead there. Guard order is fail-closed: a NaN falls into
	 * `!(Percent < Full)` and reads "no margin left".
	 */
	FString PresenterMarginText(double UtilisationPercent, bool bHasGiven)
	{
		if (bHasGiven)
		{
			return FString(TEXT("gone"));
		}

		if (!(UtilisationPercent < PresenterFullLoadPercent))
		{
			return FString(TEXT("no margin left"));
		}

		if (!(UtilisationPercent > 0.0))
		{
			return FString(TEXT("no load"));
		}

		const double MarginTimes = PresenterFullLoadPercent / UtilisationPercent;

		// The decimal is noise at 100× and above.
		return MarginTimes >= PresenterWholeMarginAtTimes
			? FString::Printf(TEXT("%.0f× margin"), MarginTimes)
			: FString::Printf(TEXT("%.1f× margin"), MarginTimes);
	}

	/**
	 * Decades of margin the log-scale headroom bar spans: full is 1000×, empty is giving. Linear
	 * would read zero everywhere, since a settled wall sits near 0.0005 of capacity.
	 */
	constexpr int32 PresenterHeadroomDecades = 3;

	/** The bar's fill for a margin, and the same curve the scale's ticks are placed by. */
	double PresenterHeadroomForMargin(double MarginTimes)
	{
		return FMath::Clamp(
			FMath::LogX(10.0, MarginTimes) / static_cast<double>(PresenterHeadroomDecades),
			0.0,
			1.0);
	}

	/**
	 * How full a joint's headroom bar is, 0 to 1. Unloaded is full, given is empty. Guards run in
	 * PresenterMarginText's order so a NaN empties the bar.
	 */
	double PresenterHeadroomFraction(double UtilisationPercent, bool bHasGiven)
	{
		if (bHasGiven)
		{
			return 0.0;
		}

		if (!(UtilisationPercent < PresenterFullLoadPercent))
		{
			return 0.0;
		}

		if (!(UtilisationPercent > 0.0))
		{
			return 1.0;
		}

		return PresenterHeadroomForMargin(PresenterFullLoadPercent / UtilisationPercent);
	}

	/**
	 * Caution and critical edges as utilisation per cent (10x and 2x margin). Per cent avoids
	 * dividing by zero for an unloaded joint.
	 */
	constexpr double PresenterCautionAtPercent = 10.0;
	constexpr double PresenterCriticalAtPercent = 50.0;

	/**
	 * The band a joint's bar is drawn in; the widget picks the hue. At an edge the joint takes
	 * the worse band. Guards run worst first and negated, so a NaN reads Critical. A given joint
	 * is Critical, since it carries nothing and would otherwise read Comfortable.
	 */
	EJointMarginBand PresenterMarginBand(double UtilisationPercent, bool bHasGiven)
	{
		if (bHasGiven)
		{
			return EJointMarginBand::Critical;
		}

		if (!(UtilisationPercent < PresenterCriticalAtPercent))
		{
			return EJointMarginBand::Critical;
		}

		if (!(UtilisationPercent < PresenterCautionAtPercent))
		{
			return EJointMarginBand::Caution;
		}

		return EJointMarginBand::Comfortable;
	}

	/** Joint rows that get a colour: six, the joints of a brick inside a running bond. */
	constexpr int32 PresenterColourSlots = 6;

	/**
	 * A joint row's colour slot, keyed on the row so two rows never share a colour. INDEX_NONE
	 * past the palette rather than wrapping, which would repeat a colour.
	 */
	int32 PresenterColourSlotFor(int32 RowIndex)
	{
		return RowIndex < PresenterColourSlots ? RowIndex : INDEX_NONE;
	}

	/**
	 * Whether both halves of a pixel measurement are finite. Needed because FMath::Max/Min
	 * silently drop a NaN, turning a fault into a plausible offset.
	 */
	bool PresenterPanelPixelsAreFinite(const FVector2D& ValuePx)
	{
		return FMath::IsFinite(ValuePx.X) && FMath::IsFinite(ValuePx.Y);
	}

	/**
	 * Whether a size is non-negative. A negative size would widen the Viewport - Panel range
	 * (fail-open). Written `>= 0.0` so a NaN fails. Zero is usable and pins to the origin.
	 */
	bool PresenterPanelSizeIsUsable(const FVector2D& SizePx)
	{
		return SizePx.X >= 0.0 && SizePx.Y >= 0.0;
	}

	/**
	 * Whether a margin is non-negative. A negative one pushes the panel past the edge, which
	 * ClampPanelOffset would then hide. Zero (flush) is usable; `>= 0.0` so a NaN fails.
	 */
	bool PresenterPanelMarginIsUsable(double MarginPx)
	{
		return MarginPx >= 0.0;
	}

	/**
	 * One axis of the panel corner, pinned to [0, LargestPx]. Max(Min(...)) so that when the panel
	 * is wider than the screen (LargestPx negative) the answer is 0, not negative. Spelled out
	 * rather than relying on FMath::Clamp's order. Pinned by Presenter.PanelOffsetClamp.
	 */
	double PresenterPanelAxisPinned(double DesiredPx, double LargestPx)
	{
		return FMath::Max(FMath::Min(DesiredPx, LargestPx), 0.0);
	}

	/*
	 * Full panel size. Width: the longest readout line (a bending clause on the corbel wall)
	 * overran 640 px by 101 px (World.Menu.TheReadoutFitsInsideThePanel), plus 39 px for the
	 * longest profile name, generalpurposemortarperpend: 780 px floor, 800 px with clearance.
	 * Height: about half a 1080 viewport. Pinned by Presenter.PieceMenuPanelSize.
	 */
	constexpr double PresenterFullPanelWidthPx = 800.0;
	constexpr double PresenterFullPanelHeightPx = 560.0;

	/*
	 * Compact panel size, its own floor rather than a fraction of Full (which grew for lines
	 * Compact does not draw). The limit is the position label overlapping the 150 px support
	 * column; per World.Menu.TheReadoutFitsInsideThePanel that happens near 231 px, so 448 px
	 * is safe. Height is 0.7 of Full, giving 39 % of Full's area.
	 */
	constexpr double PresenterCompactPanelWidthPx = 448.0;
	constexpr double PresenterCompactPanelHeightPx = 392.0;

	// The bar's decade ticks, low to high, placed by the same curve as the fill.
	TArray<FHeadroomScaleTick> PresenterHeadroomScale()
	{
		TArray<FHeadroomScaleTick> Scale;
		Scale.Reserve(PresenterHeadroomDecades + 1);

		double MarginTimes = 1.0;

		for (int32 Decade = 0; Decade <= PresenterHeadroomDecades; ++Decade)
		{
			FHeadroomScaleTick& Tick = Scale.AddDefaulted_GetRef();
			Tick.Label = FString::Printf(TEXT("%.0f×"), MarginTimes);
			Tick.Fraction = PresenterHeadroomForMargin(MarginTimes);

			MarginTimes *= 10.0;
		}

		return Scale;
	}
}

TArray<FPieceMenuRow> BuildPieceMenuRows(
	TArrayView<const FPieceAction* const> Actions,
	const FPieceRef& Ref)
{
	// A selection of one, so the fail-closed rules live in one place.
	return BuildPieceMenuRows(Actions, TArrayView<const FPieceRef>(&Ref, 1));
}

TArray<FPieceMenuRow> BuildPieceMenuRows(
	TArrayView<const FPieceAction* const> Actions,
	TArrayView<const FPieceRef> Refs)
{
	TArray<FPieceMenuRow> Rows;

	// No selection, no rows. Every row then has at least one ref, so Refs.Last() is safe below.
	if (Refs.Num() == 0)
	{
		return Rows;
	}

	/*
	 * Any ref missing a half (a click on the floor) empties the whole menu, rather than dropping
	 * that entry and acting on fewer bricks than were picked. Zero is a valid id, so test INDEX_NONE.
	 */
	for (const FPieceRef& Ref : Refs)
	{
		if (Ref.StructureId == INDEX_NONE || Ref.PieceIndex == INDEX_NONE)
		{
			return Rows;
		}
	}

	Rows.Reserve(Actions.Num());

	// One row per action, in order. CanRun is not consulted; PieceActionsFor already filtered.
	for (const FPieceAction* const Action : Actions)
	{
		// Skip malformed rows; the array may come from anywhere.
		if (Action == nullptr || Action->Label == nullptr)
		{
			continue;
		}

		FPieceMenuRow& Row = Rows.AddDefaulted_GetRef();

		// Carried by pointer: the chosen entry is identified by comparing against the shipped table.
		Row.Label = FString(Action->Label);
		Row.Action = Action;
		Row.Refs.Append(Refs.GetData(), Refs.Num());

		Row.bIsDestructive = Action->bIsDestructive;

		// Counted from Refs so the button states exactly how many bricks it acts on.
		Row.TargetText = PresenterWordForTargetCount(Row.Refs.Num());

		// Derived from the set so the anchor is always a selected piece.
		Row.Ref = Row.Refs.Last();
	}

	return Rows;
}

FPieceMenuInspector BuildPieceMenuInspector(
	const FStructureBinding& Binding,
	TArrayView<const FPieceRef> Selected,
	const FPieceRef& InspectedRef,
	EPieceMenuDetail Detail)
{
	FPieceMenuInspector Inspector;

	// Set before any early return; the heading shows in every state.
	Inspector.HeaderText = FString(PresenterPanelHeader);

	// The raw selection count, including refs that no longer resolve, to match the highlights.
	Inspector.SelectedCount = Selected.Num();

	if (Inspector.SelectedCount == 0)
	{
		Inspector.CountText = FString(TEXT("No bricks selected"));
	}
	else if (Inspector.SelectedCount == 1)
	{
		Inspector.CountText = FString(TEXT("1 brick selected"));
	}
	else
	{
		Inspector.CountText =
			FString::Printf(TEXT("%d bricks selected"), Inspector.SelectedCount);
	}

	// One entry per selected brick, in pick order (the batched commit's order), never de-duped.
	Inspector.Pieces.Reserve(Selected.Num());

	// Built once: a position depends on every other brick.
	const TArray<FString> PositionLabels = PresenterPositionLabels(Binding);

	/*
	 * Refs this binding cannot place fall back to "brick 9:1", both halves, so bricks from two
	 * walls never share a label. An unidentified binding matches nothing. ResolvePiece is not
	 * used because it rejects removed pieces, which still have a position.
	 */
	const bool bLabelsAreThisStructure = Binding.StructureId != INDEX_NONE;

	for (const FPieceRef& Ref : Selected)
	{
		FInspectorPieceEntry& Entry = Inspector.Pieces.AddDefaulted_GetRef();
		Entry.Ref = Ref;

		const bool bIsPlaced = bLabelsAreThisStructure
			&& Ref.StructureId == Binding.StructureId
			&& PositionLabels.IsValidIndex(Ref.PieceIndex)
			&& !PositionLabels[Ref.PieceIndex].IsEmpty();

		Entry.Label = bIsPlaced
			? PositionLabels[Ref.PieceIndex]
			: FString::Printf(TEXT("brick %d:%d"), Ref.StructureId, Ref.PieceIndex);

		// InspectPiece decides liveness for every entry. A released brick still reads live.
		const FPieceInspection EntryInspection = InspectPiece(Binding, Ref);

		Entry.bIsLivePiece = EntryInspection.bIsPiece;

		// Word and dot come from one bucket so they cannot disagree.
		Entry.SupportBand = PresenterSupportBand(EntryInspection);
		Entry.SupportText = PresenterWordForSupportBand(Entry.SupportBand);
	}

	// The inspected brick must be in the selection. An index marks exactly one entry.
	const int32 InspectedEntry = Selected.IndexOfByKey(InspectedRef);

	// It must also be a live piece; the default inspection (bIsPiece false) is the fail-closed answer.
	FPieceInspection Inspection;

	if (InspectedEntry != INDEX_NONE)
	{
		Inspection = InspectPiece(Binding, InspectedRef);
	}

	Inspector.bHasInspectedPiece = Inspection.bIsPiece;

	// The hint shows only when bricks are selected but none is inspected.
	if (Inspector.SelectedCount > 0 && !Inspector.bHasInspectedPiece)
	{
		Inspector.InspectedHintText = FString(PresenterInspectedHint);
	}

	if (!Inspector.bHasInspectedPiece)
	{
		return Inspector;
	}

	Inspector.Pieces[InspectedEntry].bIsInspected = true;
	Inspector.InspectedRef = InspectedRef;

	// Copied from the entry, since the list may be scrolled away from it.
	Inspector.InspectedLabel = Inspector.Pieces[InspectedEntry].Label;

	/*
	 * Material, size and mass. Composed here because the widget was allowed to skip TDD only on
	 * condition it holds no logic. Uses InspectPiece's resolved handle, never the ref.
	 */
	Inspector.IdentityText = PresenterIdentityLine(Binding, Inspection.PieceIndex);

	Inspector.SupportBand = PresenterSupportBand(Inspection);
	Inspector.SupportText = PresenterWordForSupportBand(Inspector.SupportBand);

	Inspector.JointsText = PresenterWordForJointCount(Inspection.Joints.Num());

	/*
	 * Compact stops before the joint table and scale, and before building them at all. The
	 * joint count above is the brick's real count, not a trimmed list's.
	 */
	if (Detail == EPieceMenuDetail::Compact)
	{
		return Inspector;
	}

	/*
	 * InspectPiece's joints, converted and worded in its order; no number is re-derived. Only
	 * the profile is read off the graph, since the inspection does not carry it.
	 */
	Inspector.Joints.Reserve(Inspection.Joints.Num());

	for (int32 JointIndex = 0; JointIndex < Inspection.Joints.Num(); ++JointIndex)
	{
		const FJointInspection& Joint = Inspection.Joints[JointIndex];

		FInspectorJointRow& Row = Inspector.Joints.AddDefaulted_GetRef();

		Row.ColourSlot = PresenterColourSlotFor(JointIndex);

		Row.ConnectionIndex = Joint.ConnectionIndex;
		Row.OtherPieceIndex = Joint.OtherPieceIndex;
		Row.Role = Joint.Role;
		Row.bHasGiven = Joint.bHasGiven;
		Row.BreakPass = Joint.BreakPass;

		/*
		 * The one unit change: 1 N = 100 uu (ForceUnitsPerNewton, not ForceUnitsPerMPaSqCm,
		 * which is 100x more). Divided, since 0.01 is not exactly representable.
		 */
		Row.ForceN = Joint.ForceUu.Size() / DestructionPresenter::ForceUnitsPerNewton;

		// Same conversion: a moment is uu.cm and cm needs no change.
		Row.MomentNCm = Joint.MomentUuCm.Size() / DestructionPresenter::ForceUnitsPerNewton;

		Row.UtilisationPercent = Joint.Utilisation * 100.0;

		// Derived from the row's own numbers so they cannot disagree with the per cent.
		Row.MarginText = PresenterMarginText(Row.UtilisationPercent, Row.bHasGiven);
		Row.HeadroomFraction = PresenterHeadroomFraction(Row.UtilisationPercent, Row.bHasGiven);
		Row.MarginBand = PresenterMarginBand(Row.UtilisationPercent, Row.bHasGiven);

		/*
		 * The far end is named from the same table as the entry list, so a brick has one name
		 * across the panel; a removed far end still has a position. The fallback matches the
		 * entry label and uses the binding's id, since a far end is always in this structure.
		 */
		const bool bFarEndIsPlaced = PositionLabels.IsValidIndex(Row.OtherPieceIndex)
			&& !PositionLabels[Row.OtherPieceIndex].IsEmpty();

		const FString OtherPieceText = bFarEndIsPlaced
			? PositionLabels[Row.OtherPieceIndex]
			: FString::Printf(TEXT("brick %d:%d"), Binding.StructureId, Row.OtherPieceIndex);

		/*
		 * A given joint gets its own sentence, since it would otherwise read 0 N at 0 % like an
		 * unloaded one. The profile is read off the graph; FJointInspection does not carry it.
		 */
		const FString ProfileWord =
			PresenterWordForJointProfile(Binding.GetStructure(), Row.ConnectionIndex);

		Row.Text = Row.bHasGiven
			? FString::Printf(
				TEXT("#%d  %s  %s  broken (went with a removed piece)"),
				Row.ConnectionIndex, *OtherPieceText,
				PresenterWordForJointRole(Row.Role))
			: FString::Printf(
				TEXT("#%d  %s  %s  %s  %s  %.3f %%  %s%s"),
				Row.ConnectionIndex, *OtherPieceText,
				PresenterWordForJointRole(Row.Role), *ProfileWord,
				*PresenterForceText(Row.ForceN), Row.UtilisationPercent, *Row.MarginText,
				*PresenterBendingText(Row.MomentNCm));
	}

	// Caption and ticks only when there are bars. The caption quotes the scale's top tick.
	if (Inspector.Joints.Num() > 0)
	{
		Inspector.HeadroomScale = PresenterHeadroomScale();

		Inspector.HeadroomCaption = FString::Printf(
			TEXT("headroom — full is %s margin, empty is the joint giving"),
			*Inspector.HeadroomScale.Last().Label);
	}

	return Inspector;
}

FVector2D PieceMenuPanelSizePx(EPieceMenuDetail Detail)
{
	/*
	 * Anything but Compact, including an undeclared cast value, gets Full: hiding data is the
	 * worse failure. The switch stays exhaustive so a new mode warns here.
	 */
	switch (Detail)
	{
	case EPieceMenuDetail::Compact:
		return FVector2D(PresenterCompactPanelWidthPx, PresenterCompactPanelHeightPx);

	case EPieceMenuDetail::Full:
		break;
	}

	return FVector2D(PresenterFullPanelWidthPx, PresenterFullPanelHeightPx);
}

FVector2D PieceMenuHomeOffset(
	FVector2D PanelSizePx,
	FVector2D ViewportSizePx,
	double MarginPx)
{
	// Any non-finite input fails the whole vector to the origin (FMath::Max would hide a NaN).
	if (!PresenterPanelPixelsAreFinite(PanelSizePx)
		|| !PresenterPanelPixelsAreFinite(ViewportSizePx)
		|| !FMath::IsFinite(MarginPx))
	{
		return FVector2D::ZeroVector;
	}

	// Negative sizes or margin would widen the room below (fail-open).
	if (!PresenterPanelSizeIsUsable(PanelSizePx)
		|| !PresenterPanelSizeIsUsable(ViewportSizePx)
		|| !PresenterPanelMarginIsUsable(MarginPx))
	{
		return FVector2D::ZeroVector;
	}

	/*
	 * Right edge MarginPx in from the viewport's, vertically centred. Each axis is pinned at
	 * zero so the heading stays on screen; no upper pin is needed since the margin is
	 * non-negative.
	 */
	return FVector2D(
		FMath::Max(ViewportSizePx.X - PanelSizePx.X - MarginPx, 0.0),
		FMath::Max((ViewportSizePx.Y - PanelSizePx.Y) * 0.5, 0.0));
}

FVector2D ClampPanelOffset(
	FVector2D DesiredOffsetPx,
	FVector2D PanelSizePx,
	FVector2D ViewportSizePx)
{
	// Any non-finite input fails the whole vector to the origin, which is always on screen.
	if (!PresenterPanelPixelsAreFinite(DesiredOffsetPx)
		|| !PresenterPanelPixelsAreFinite(PanelSizePx)
		|| !PresenterPanelPixelsAreFinite(ViewportSizePx))
	{
		return FVector2D::ZeroVector;
	}

	if (!PresenterPanelSizeIsUsable(PanelSizePx) || !PresenterPanelSizeIsUsable(ViewportSizePx))
	{
		return FVector2D::ZeroVector;
	}

	// Axes pinned independently so a drag past one edge does not reset the other axis.
	return FVector2D(
		PresenterPanelAxisPinned(DesiredOffsetPx.X, ViewportSizePx.X - PanelSizePx.X),
		PresenterPanelAxisPinned(DesiredOffsetPx.Y, ViewportSizePx.Y - PanelSizePx.Y));
}

EJointMarginBand WorstJointBandForPiece(const FStructure& Structure, int32 PieceIndex)
{
	// IsPieceRemoved is also true for any invalid handle.
	if (Structure.IsPieceRemoved(PieceIndex))
	{
		return EJointMarginBand::Critical;
	}

	/*
	 * Unsolved is not comfortable: utilisation reads zero before any solve, which would paint
	 * the wall green. HasSupportAnswer distinguishes the two.
	 */
	if (!Structure.HasSupportAnswer(PieceIndex))
	{
		return EJointMarginBand::Critical;
	}

	// An unsupported piece is Critical even though its remaining joints read nearly unloaded.
	if (!Structure.IsPieceSupported(PieceIndex))
	{
		return EJointMarginBand::Critical;
	}

	/*
	 * The worst band over the piece's joints, each bucketed by PresenterMarginBand. Taking a
	 * max of utilisations first would drop a NaN (DESIGN §4). Given joints are skipped, or
	 * every neighbour of a deleted brick would read red.
	 */
	EJointMarginBand Worst = EJointMarginBand::Comfortable;
	bool bAnyLiveJoint = false;

	for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
	{
		const FConnection& Connection = Structure.GetConnection(Index);

		if (Connection.PieceA != PieceIndex && Connection.PieceB != PieceIndex)
		{
			continue;
		}

		if (Connection.HasGiven())
		{
			continue;
		}

		bAnyLiveJoint = true;

		// bHasGiven is false because the given ones are already gone (skipped above).
		const EJointMarginBand Band = PresenterMarginBand(
			100.0 * Structure.GetConnectionUtilisation(Index), /*bHasGiven*/ false);

		if (static_cast<uint8>(Band) < static_cast<uint8>(Worst))
		{
			Worst = Band;
		}
	}

	// A supported piece with no live joints (e.g. a lone brick on the ground) is comfortable.
	return bAnyLiveJoint ? Worst : EJointMarginBand::Comfortable;
}

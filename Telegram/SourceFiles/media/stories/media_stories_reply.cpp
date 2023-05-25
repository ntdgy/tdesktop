/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/stories/media_stories_reply.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "boxes/premium_limits_box.h"
#include "boxes/send_files_box.h"
#include "chat_helpers/compose/compose_show.h"
#include "chat_helpers/tabbed_selector.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/view/controls/compose_controls_common.h"
#include "history/view/controls/history_view_compose_controls.h"
#include "history/history_item_helpers.h"
#include "history/history.h"
#include "inline_bots/inline_bot_result.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/stories/media_stories_controller.h"
#include "menu/menu_send.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "styles/style_boxes.h" // sendMediaPreviewSize.
#include "styles/style_chat_helpers.h"
#include "styles/style_media_view.h"

namespace Media::Stories {

ReplyArea::ReplyArea(not_null<Controller*> controller)
: _controller(controller)
, _controls(std::make_unique<HistoryView::ComposeControls>(
	_controller->wrap(),
	HistoryView::ComposeControlsDescriptor{
		.stOverride = &st::storiesComposeControls,
		.show = _controller->uiShow(),
		.unavailableEmojiPasted = [=](not_null<DocumentData*> emoji) {
			showPremiumToast(emoji);
		},
		.mode = HistoryView::ComposeControlsMode::Normal,
		.sendMenuType = SendMenu::Type::SilentOnly,
		.stickerOrEmojiChosen = _controller->stickerOrEmojiChosen(),
		.voiceLockFromBottom = true,
		.features = {
			.sendAs = false,
			.ttlInfo = false,
			.botCommandSend = false,
			.silentBroadcastToggle = false,
			.attachBotsMenu = false,
			.inlineBots = false,
			.megagroupSet = false,
			.stickersSettings = false,
			.openStickerSets = false,
			.autocompleteHashtags = false,
			.autocompleteMentions = false,
			.autocompleteCommands = false,
		},
	}
)) {
	initGeometry();
	initActions();
}

ReplyArea::~ReplyArea() {
}

void ReplyArea::initGeometry() {
	rpl::combine(
		_controller->layoutValue(),
		_controls->height()
	) | rpl::start_with_next([=](const Layout &layout, int height) {
		const auto content = layout.content;
		_controls->resizeToWidth(layout.controlsWidth);
		if (_controls->heightCurrent() == height) {
			const auto position = layout.controlsBottomPosition
				- QPoint(0, height);
			_controls->move(position.x(), position.y());
			const auto &tabbed = st::storiesComposeControls.tabbed;
			const auto upper = QRect(
				position.x(),
				content.y(),
				layout.controlsWidth,
				(position.y()
					+ tabbed.autocompleteBottomSkip
					- content.y()));
			_controls->setAutocompleteBoundingRect(
				layout.autocompleteRect.intersected(upper));
		}
	}, _lifetime);
}

void ReplyArea::send(Api::SendOptions options) {
	const auto webPageId = _controls->webPageId();

	auto message = ApiWrap::MessageToSend(prepareSendAction(options));
	message.textWithTags = _controls->getTextWithAppliedMarkdown();
	message.webPageId = webPageId;

	const auto error = GetErrorTextForSending(
		_data.user,
		{
			.topicRootId = MsgId(0),
			.text = &message.textWithTags,
			.ignoreSlowmodeCountdown = (options.scheduled != 0),
		});
	if (!error.isEmpty()) {
		_controller->uiShow()->showToast(error);
	}

	session().api().sendMessage(std::move(message));

	_controls->clear();
	finishSending();
}

void ReplyArea::sendVoice(VoiceToSend &&data) {
	auto action = prepareSendAction(data.options);
	session().api().sendVoiceMessage(
		data.bytes,
		data.waveform,
		data.duration,
		std::move(action));

	_controls->clearListenState();
	finishSending();
}

void ReplyArea::finishSending() {
	_controls->hidePanelsAnimated();
	_controller->wrap()->setFocus();
}

void ReplyArea::uploadFile(
		const QByteArray &fileContent,
		SendMediaType type) {
	session().api().sendFile(fileContent, type, prepareSendAction({}));
}

bool ReplyArea::showSendingFilesError(
		const Ui::PreparedList &list) const {
	return showSendingFilesError(list, std::nullopt);
}

bool ReplyArea::showSendingFilesError(
		const Ui::PreparedList &list,
		std::optional<bool> compress) const {
	const auto text = [&] {
		const auto peer = _data.user;
		const auto error = Data::FileRestrictionError(peer, list, compress);
		if (error) {
			return *error;
		}
		using Error = Ui::PreparedList::Error;
		switch (list.error) {
		case Error::None: return QString();
		case Error::EmptyFile:
		case Error::Directory:
		case Error::NonLocalUrl: return tr::lng_send_image_empty(
			tr::now,
			lt_name,
			list.errorData);
		case Error::TooLargeFile: return u"(toolarge)"_q;
		}
		return tr::lng_forward_send_files_cant(tr::now);
	}();
	if (text.isEmpty()) {
		return false;
	} else if (text == u"(toolarge)"_q) {
		const auto fileSize = list.files.back().size;
		_controller->uiShow()->showBox(Box(
			FileSizeLimitBox,
			&session(),
			fileSize,
			&st::storiesComposePremium));
		return true;
	}

	_controller->uiShow()->showToast(text);
	return true;
}

Api::SendAction ReplyArea::prepareSendAction(
		Api::SendOptions options) const {
	Expects(_data.user != nullptr);

	const auto history = _data.user->owner().history(_data.user);
	auto result = Api::SendAction(history, options);
	result.options.sendAs = _controls->sendAsPeer();
	result.replyTo.storyId = { .peer = _data.user->id, .story = _data.id };
	return result;
}

void ReplyArea::chooseAttach(
		std::optional<bool> overrideSendImagesAsPhotos) {
	if (!_data.user) {
		return;
	}
	const auto user = not_null(_data.user);
	_choosingAttach = false;
	if (const auto error = Data::AnyFileRestrictionError(user)) {
		_controller->uiShow()->showToast(*error);
		return;
	}

	const auto filter = (overrideSendImagesAsPhotos == true)
		? FileDialog::ImagesOrAllFilter()
		: FileDialog::AllOrImagesFilter();
	const auto callback = [=](FileDialog::OpenResult &&result) {
		if (result.paths.isEmpty() && result.remoteContent.isEmpty()) {
			return;
		}

		if (!result.remoteContent.isEmpty()) {
			auto read = Images::Read({
				.content = result.remoteContent,
				});
			if (!read.image.isNull() && !read.animated) {
				confirmSendingFiles(
					std::move(read.image),
					std::move(result.remoteContent),
					overrideSendImagesAsPhotos);
			} else {
				uploadFile(result.remoteContent, SendMediaType::File);
			}
		} else {
			const auto premium = session().premium();
			auto list = Storage::PrepareMediaList(
				result.paths,
				st::sendMediaPreviewSize,
				premium);
			list.overrideSendImagesAsPhotos = overrideSendImagesAsPhotos;
			confirmSendingFiles(std::move(list));
		}
	};
	FileDialog::GetOpenPaths(
		_controller->wrap().get(),
		tr::lng_choose_files(tr::now),
		filter,
		crl::guard(&_shownUserGuard, callback),
		nullptr);
}

bool ReplyArea::confirmSendingFiles(
		not_null<const QMimeData*> data,
		std::optional<bool> overrideSendImagesAsPhotos,
		const QString &insertTextOnCancel) {
	const auto hasImage = data->hasImage();
	const auto premium = session().user()->isPremium();

	if (const auto urls = Core::ReadMimeUrls(data); !urls.empty()) {
		auto list = Storage::PrepareMediaList(
			urls,
			st::sendMediaPreviewSize,
			premium);
		if (list.error != Ui::PreparedList::Error::NonLocalUrl) {
			if (list.error == Ui::PreparedList::Error::None
				|| !hasImage) {
				const auto emptyTextOnCancel = QString();
				list.overrideSendImagesAsPhotos = overrideSendImagesAsPhotos;
				confirmSendingFiles(std::move(list), emptyTextOnCancel);
				return true;
			}
		}
	}

	if (auto read = Core::ReadMimeImage(data)) {
		confirmSendingFiles(
			std::move(read.image),
			std::move(read.content),
			overrideSendImagesAsPhotos,
			insertTextOnCancel);
		return true;
	}
	return false;
}

bool ReplyArea::confirmSendingFiles(
		Ui::PreparedList &&list,
		const QString &insertTextOnCancel) {
	if (_controls->confirmMediaEdit(list)) {
		return true;
	} else if (showSendingFilesError(list)) {
		return false;
	}

	const auto show = _controller->uiShow();
	auto confirmed = [=](auto &&...args) {
		sendingFilesConfirmed(std::forward<decltype(args)>(args)...);
	};
	auto box = Box<SendFilesBox>(SendFilesBoxDescriptor{
		.show = show,
		.list = std::move(list),
		.caption = _controls->getTextWithAppliedMarkdown(),
		.limits = DefaultLimitsForPeer(_data.user),
		.check = DefaultCheckForPeer(show, _data.user),
		.sendType = Api::SendType::Normal,
		.sendMenuType = SendMenu::Type::SilentOnly,
		.stOverride = &st::storiesComposeControls,
		.confirmed = crl::guard(this, confirmed),
		.cancelled = _controls->restoreTextCallback(insertTextOnCancel),
	});
	if (const auto shown = show->show(std::move(box))) {
		shown->setCloseByOutsideClick(false);
	}

	return true;
}

void ReplyArea::sendingFilesConfirmed(
		Ui::PreparedList &&list,
		Ui::SendFilesWay way,
		TextWithTags &&caption,
		Api::SendOptions options,
		bool ctrlShiftEnter) {
	Expects(list.filesToProcess.empty());

	if (showSendingFilesError(list, way.sendImagesAsPhotos())) {
		return;
	}
	auto groups = DivideByGroups(
		std::move(list),
		way,
		_data.user->slowmodeApplied());
	const auto type = way.sendImagesAsPhotos()
		? SendMediaType::Photo
		: SendMediaType::File;
	auto action = prepareSendAction(options);
	action.clearDraft = false;
	if ((groups.size() != 1 || !groups.front().sentWithCaption())
		&& !caption.text.isEmpty()) {
		auto message = Api::MessageToSend(action);
		message.textWithTags = base::take(caption);
		session().api().sendMessage(std::move(message));
	}
	for (auto &group : groups) {
		const auto album = (group.type != Ui::AlbumType::None)
			? std::make_shared<SendingAlbum>()
			: nullptr;
		session().api().sendFiles(
			std::move(group.list),
			type,
			base::take(caption),
			album,
			action);
	}
	finishSending();
}

bool ReplyArea::confirmSendingFiles(
		QImage &&image,
		QByteArray &&content,
		std::optional<bool> overrideSendImagesAsPhotos,
		const QString &insertTextOnCancel) {
	if (image.isNull()) {
		return false;
	}

	auto list = Storage::PrepareMediaFromImage(
		std::move(image),
		std::move(content),
		st::sendMediaPreviewSize);
	list.overrideSendImagesAsPhotos = overrideSendImagesAsPhotos;
	return confirmSendingFiles(std::move(list), insertTextOnCancel);
}

void ReplyArea::initActions() {
	_controls->cancelRequests(
	) | rpl::start_with_next([=] {
		_controller->unfocusReply();
	}, _lifetime);

	_controls->sendRequests(
	) | rpl::start_with_next([=](Api::SendOptions options) {
		send(options);
	}, _lifetime);

	_controls->sendVoiceRequests(
	) | rpl::start_with_next([=](VoiceToSend &&data) {
		sendVoice(std::move(data));
	}, _lifetime);

	_controls->attachRequests(
	) | rpl::filter([=] {
		return !_choosingAttach;
	}) | rpl::start_with_next([=](std::optional<bool> overrideCompress) {
		_choosingAttach = true;
		base::call_delayed(
			st::storiesAttach.ripple.hideDuration,
			this,
			[=] { chooseAttach(overrideCompress); });
	}, _lifetime);

	_controls->fileChosen(
	) | rpl::start_with_next([=](ChatHelpers::FileChosen data) {
		_controller->uiShow()->hideLayer();
		//controller()->sendingAnimation().appendSending(
		//	data.messageSendingFrom);
		//const auto localId = data.messageSendingFrom.localId;
		//sendExistingDocument(data.document, data.options, localId);
	}, _lifetime);

	_controls->photoChosen(
	) | rpl::start_with_next([=](ChatHelpers::PhotoChosen chosen) {
		//sendExistingPhoto(chosen.photo, chosen.options);
	}, _lifetime);

	_controls->inlineResultChosen(
	) | rpl::start_with_next([=](ChatHelpers::InlineChosen chosen) {
		//controller()->sendingAnimation().appendSending(
		//	chosen.messageSendingFrom);
		//const auto localId = chosen.messageSendingFrom.localId;
		//sendInlineResult(chosen.result, chosen.bot, chosen.options, localId);
	}, _lifetime);

	_controls->setMimeDataHook([=](
			not_null<const QMimeData*> data,
			Ui::InputField::MimeAction action) {
		if (action == Ui::InputField::MimeAction::Check) {
			return false;// checkSendingFiles(data);
		} else if (action == Ui::InputField::MimeAction::Insert) {
			return false;/* confirmSendingFiles(
				data,
				std::nullopt,
				Core::ReadMimeText(data));*/
		}
		Unexpected("action in MimeData hook.");
	});

	_controls->lockShowStarts(
	) | rpl::start_with_next([=] {
	}, _lifetime);

	_controls->show();
	_controls->finishAnimating();
	_controls->showFinished();
}

void ReplyArea::show(ReplyAreaData data) {
	if (_data == data) {
		return;
	}
	const auto userChanged = (_data.user != data.user);
	_data = data;
	if (!userChanged) {
		if (_data.user) {
			_controls->clear();
		}
		return;
	}
	invalidate_weak_ptrs(&_shownUserGuard);
	const auto user = data.user;
	const auto history = user ? user->owner().history(user).get() : nullptr;
	_controls->setHistory({
		.history = history,
	});
	_controls->clear();
}

Main::Session &ReplyArea::session() const {
	Expects(_data.user != nullptr);

	return _data.user->session();
}

rpl::producer<bool> ReplyArea::focusedValue() const {
	return _controls->focusedValue();
}

void ReplyArea::showPremiumToast(not_null<DocumentData*> emoji) {
	// #TODO stories
}

} // namespace Media::Stories

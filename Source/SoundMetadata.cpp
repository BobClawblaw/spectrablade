#include "SoundMetadata.h"
#include "SoundCategories.h"
#include <cstdio>
#include <filesystem>

namespace SoundMetadata
{
    namespace
    {
        juce::String idOf (juce::uint32 littleEndian)
        {
            char text[5] = {};

            for (int i = 0; i < 4; ++i)
                text[i] = (char) ((littleEndian >> (8 * i)) & 0xff);

            return juce::String (juce::CharPointer_ASCII (text), 4);
        }

        /** The file's chunks, or why it can't be written. */
        juce::String parse (juce::InputStream& in, juce::int64 fileSize, std::vector<Chunk>& chunks)
        {
            const auto riff = (juce::uint32) in.readInt();
            const auto riffSize = (juce::int64) (juce::uint32) in.readInt();
            const auto wave = (juce::uint32) in.readInt();

            if (riff == juce::ByteOrder::littleEndianInt ("RF64") || riff == juce::ByteOrder::littleEndianInt ("BW64"))
                return "it is an RF64 file, whose sizes this doesn't rewrite";

            if (riff != juce::ByteOrder::littleEndianInt ("RIFF") || wave != juce::ByteOrder::littleEndianInt ("WAVE"))
                return "it isn't a WAV file";

            const auto end = juce::jmin (fileSize, riffSize + 8);
            juce::int64 at = 12;

            while (at + 8 <= end)
            {
                in.setPosition (at);
                const auto id = idOf ((juce::uint32) in.readInt());
                const auto size = (juce::int64) (juce::uint32) in.readInt();

                if (at + 8 + size > fileSize)
                    return "its " + id.trim() + " chunk runs past the end of the file";

                chunks.push_back ({ id, at + 8, size });
                at += 8 + size + (size & 1);
            }

            for (const auto& c : chunks)
                if (c.id == "data")
                    return {};

            return "it has no audio (data) chunk";
        }

        void setChild (juce::XmlElement& parent, const char* tag, const juce::String& value)
        {
            auto* child = parent.getChildByName (tag);

            if (child == nullptr)
                child = parent.createNewChildElement (tag);

            child->deleteAllChildElements();
            child->addTextElement (value);
        }

        juce::XmlElement& childOf (juce::XmlElement& parent, const char* tag)
        {
            if (auto* child = parent.getChildByName (tag))
                return *child;

            return *parent.createNewChildElement (tag);
        }

        /** The iXML with the fields set in it, or an empty string with why in the error. */
        juce::String newIXml (const juce::String& old, const Fields& fields, juce::String& error)
        {
            std::unique_ptr<juce::XmlElement> doc;

            if (old.trim().isEmpty())
            {
                doc = std::make_unique<juce::XmlElement> ("BWFXML");
                setChild (*doc, "IXML_VERSION", "2.10");
            }
            else
            {
                doc = juce::parseXML (old);

                if (doc == nullptr || ! doc->hasTagName ("BWFXML"))
                {
                    error = "its iXML chunk isn't iXML that can be read";
                    return {};
                }
            }

            auto& user = childOf (*doc, "USER");

            if (const auto* e = Ucs::byCatId (fields.catId))
            {
                setChild (user, "CATID", e->catId);
                setChild (user, "CATEGORY", e->category);
                setChild (user, "SUBCATEGORY", e->subCategory);
                setChild (user, "CATEGORYFULL", e->category + "-" + e->subCategory);
                auto& aswg = childOf (*doc, "ASWG");
                setChild (aswg, "catId", e->catId);
                setChild (aswg, "category", e->category);
                setChild (aswg, "subCategory", e->subCategory);
            }

            if (! fields.tags.isEmpty())
                setChild (user, "KEYWORDS", fields.tags.joinIntoString (", "));

            return doc->toString();
        }

        /** The fields this writes, as leaf paths. */
        bool isWrittenField (const juce::String& path)
        {
            static const juce::StringArray fields { "BWFXML/USER/CATID", "BWFXML/USER/CATEGORY", "BWFXML/USER/SUBCATEGORY",
                                                    "BWFXML/USER/CATEGORYFULL", "BWFXML/USER/KEYWORDS", "BWFXML/ASWG/catId",
                                                    "BWFXML/ASWG/category", "BWFXML/ASWG/subCategory" };
            return fields.contains (path);
        }

        /** Every element's path with its attributes and own text, but those this writes; sorted. */
        juce::StringArray contentsOf (const juce::XmlElement& e, const juce::String& parent = {})
        {
            juce::StringArray all;
            const auto path = parent + e.getTagName();

            if (isWrittenField (path))
                return all;

            juce::String line (path);

            for (int i = 0; i < e.getNumAttributes(); ++i)
                line << " " << e.getAttributeName (i) << "=" << e.getAttributeValue (i);

            juce::String text;

            for (auto* child : e.getChildIterator())
                if (child->isTextElement())
                    text << child->getText();

            line << " : " << text.trim();

            // The USER and ASWG blocks may be added; only what is in them counts.
            if (line != "BWFXML/USER : " && line != "BWFXML/ASWG : ")
                all.add (line);

            for (auto* child : e.getChildIterator())
                if (! child->isTextElement())
                    all.addArray (contentsOf (*child, path + "/"));

            all.sort (false);
            return all;
        }

        /** Why an iXML chunk's bytes can't be rewritten as text, or nothing. */
        juce::String notText (const juce::MemoryBlock& block)
        {
            auto size = block.getSize();
            const auto* bytes = static_cast<const unsigned char*> (block.getData());

            while (size > 0 && (bytes[size - 1] == 0 || bytes[size - 1] == ' ' || bytes[size - 1] == '\r' || bytes[size - 1] == '\n'))
                --size;

            for (size_t i = 0; i < size; ++i)
                if (bytes[i] < 32 && bytes[i] != '\t' && bytes[i] != '\n' && bytes[i] != '\r')
                    return "its iXML chunk holds bytes that aren't text, which rewriting would lose";

            if (! juce::CharPointer_UTF8::isValidString (reinterpret_cast<const char*> (bytes), (int) size))
                return "its iXML chunk isn't UTF-8 text";

            return {};
        }

        bool copyBytes (juce::InputStream& in, juce::int64 start, juce::int64 size, juce::OutputStream& out)
        {
            in.setPosition (start);
            return out.writeFromInputStream (in, size) == size;
        }

        bool sameBytes (juce::InputStream& a, juce::int64 aStart, juce::InputStream& b, juce::int64 bStart, juce::int64 size)
        {
            a.setPosition (aStart);
            b.setPosition (bStart);
            juce::HeapBlock<char> x (1 << 20), y (1 << 20);

            for (juce::int64 done = 0; done < size;)
            {
                const auto n = (int) juce::jmin ((juce::int64) 1 << 20, size - done);

                if (a.read (x, n) != n || b.read (y, n) != n || std::memcmp (x, y, (size_t) n) != 0)
                    return false;

                done += n;
            }

            return true;
        }
    }

    std::vector<Chunk> chunksOf (const juce::File& file)
    {
        std::vector<Chunk> chunks;
        juce::FileInputStream in (file);

        if (! in.openedOk() || parse (in, file.getSize(), chunks).isNotEmpty())
            return {};

        return chunks;
    }

    juce::String write (const juce::File& wav, const Fields& fields)
    {
        if (Ucs::byCatId (fields.catId) == nullptr && fields.tags.isEmpty())
            return "there is no category or tag to write";

        juce::FileInputStream in (wav);

        if (! in.openedOk())
            return "it can't be read";

        if (! wav.hasWriteAccess())
            return "it can't be written to";

        std::vector<Chunk> chunks;

        if (const auto why = parse (in, wav.getSize(), chunks); why.isNotEmpty())
            return why;

        // The new iXML, from the old one (if any).
        juce::String old;
        auto ixmlAt = -1;
        std::unique_ptr<juce::XmlElement> oldDoc;

        for (size_t i = 0; i < chunks.size(); ++i)
        {
            if (chunks[i].id == "iXML")
            {
                ixmlAt = (int) i;
                in.setPosition (chunks[i].start);
                juce::MemoryBlock block;
                in.readIntoMemoryBlock (block, (ssize_t) chunks[i].size);

                if (const auto why = notText (block); why.isNotEmpty())
                    return why;

                old = juce::String::fromUTF8 (static_cast<const char*> (block.getData()), (int) block.getSize()).trim();
                oldDoc = juce::parseXML (old);
                break;
            }
        }

        juce::String error;
        const auto xml = newIXml (old, fields, error);

        if (xml.isEmpty())
            return error;

        const auto text = xml.toUTF8();
        const auto ixmlSize = (juce::int64) std::strlen (text);
        juce::int64 total = 4;

        for (size_t i = 0; i < chunks.size(); ++i)
        {
            const auto size = (int) i == ixmlAt ? ixmlSize : chunks[i].size;
            total += 8 + size + (size & 1);
        }

        if (ixmlAt < 0)
            total += 8 + ixmlSize + (ixmlSize & 1);

        if (total > 0xfffffff0LL)
            return "it would pass 4 GB";

        // The new file, beside the old one.
        const auto temp = wav.getSiblingFile ("." + wav.getFileName() + ".spectrablade-partial");
        temp.deleteFile();
        const auto failed = [&temp] (const juce::String& why) { temp.deleteFile(); return why; };
        std::vector<Chunk> written;
        {
            juce::FileOutputStream out (temp);

            if (! out.openedOk())
                return failed ("its folder can't be written to");

            out.writeInt ((int) juce::ByteOrder::littleEndianInt ("RIFF"));
            out.writeInt ((int) (juce::uint32) total);
            out.writeInt ((int) juce::ByteOrder::littleEndianInt ("WAVE"));

            const auto writeIXml = [&]
            {
                out.writeInt ((int) juce::ByteOrder::littleEndianInt ("iXML"));
                out.writeInt ((int) (juce::uint32) ixmlSize);
                written.push_back ({ "iXML", out.getPosition(), ixmlSize });
                out.write (text, (size_t) ixmlSize);

                if ((ixmlSize & 1) != 0)
                    out.writeByte (0);
            };

            for (size_t i = 0; i < chunks.size(); ++i)
            {
                if ((int) i == ixmlAt)
                {
                    writeIXml();
                    continue;
                }

                const auto& c = chunks[i];
                out.writeInt ((int) juce::ByteOrder::littleEndianInt (c.id.toRawUTF8()));
                out.writeInt ((int) (juce::uint32) c.size);
                written.push_back ({ c.id, out.getPosition(), c.size });

                if (! copyBytes (in, c.start, c.size, out))
                    return failed ("it couldn't be copied");

                if ((c.size & 1) != 0)
                    out.writeByte (0);
            }

            if (ixmlAt < 0)
                writeIXml();

            out.flush();

            if (out.getStatus().failed())
                return failed ("the new file couldn't be written: " + out.getStatus().getErrorMessage());
        }

        // Read back and checked: every chunk but the iXML the same as the old one's, the iXML what was meant.
        {
            juce::FileInputStream check (temp);
            std::vector<Chunk> again;

            if (! check.openedOk() || parse (check, temp.getSize(), again).isNotEmpty() || again.size() != written.size())
                return failed ("the new file didn't read back as written");

            std::vector<int> from;   // for each chunk written, the old chunk it copies (-1: the new iXML)

            for (size_t i = 0; i < chunks.size(); ++i)
                from.push_back ((int) i == ixmlAt ? -1 : (int) i);

            if (ixmlAt < 0)
                from.push_back (-1);

            for (size_t k = 0; k < again.size(); ++k)
            {
                if (again[k].id != written[k].id || again[k].size != written[k].size)
                    return failed ("the new file didn't read back as written");

                if (from[k] >= 0)
                {
                    const auto& c = chunks[(size_t) from[k]];

                    if (! sameBytes (in, c.start, check, again[k].start, c.size))
                        return failed ("the new file's " + c.id.trim() + " chunk didn't match the old one's");

                    continue;
                }

                check.setPosition (again[k].start);
                juce::MemoryBlock block;
                check.readIntoMemoryBlock (block, (ssize_t) again[k].size);
                const auto doc = juce::parseXML (block.toString());
                const auto* user = doc != nullptr ? doc->getChildByName ("USER") : nullptr;
                const auto* keywords = user != nullptr ? user->getChildByName ("KEYWORDS") : nullptr;
                const auto* meant = Ucs::byCatId (fields.catId);

                if (doc == nullptr || (meant != nullptr && Ucs::fromIXml (block.toString()) != meant)
                    || (oldDoc != nullptr && contentsOf (*oldDoc) != contentsOf (*doc))
                    || (! fields.tags.isEmpty() && (keywords == nullptr || keywords->getAllSubText() != fields.tags.joinIntoString (", "))))
                    return failed ("the new file's iXML didn't read back as written");
            }
        }

        // Read as WAV whatever the names say (the temporary file's doesn't end in .wav).
        juce::WavAudioFormat format;
        const auto readerFor = [&format] (const juce::File& f)
        {
            return std::unique_ptr<juce::AudioFormatReader> (format.createReaderFor (f.createInputStream().release(), true));
        };
        auto before = readerFor (wav), after = readerFor (temp);

        if (after == nullptr || (before != nullptr && (after->lengthInSamples != before->lengthInSamples
                                                       || ! juce::exactlyEqual (after->sampleRate, before->sampleRate)
                                                       || after->numChannels != before->numChannels)))
            return failed ("the new file's audio didn't read as the old one's");

        // The new file, with the old one's permissions, in its place in one step.
        before.reset();
        after.reset();
        std::error_code ignored;
        std::filesystem::permissions (temp.getFullPathName().toStdString(),
                                      std::filesystem::status (wav.getFullPathName().toStdString(), ignored).permissions(), ignored);

       #if JUCE_WINDOWS
        // rename won't replace a file on Windows; ReplaceFile does, in one step, keeping its attributes.
        if (! temp.replaceFileIn (wav))
            return failed ("it couldn't be replaced");
       #else
        if (std::rename (temp.getFullPathName().toRawUTF8(), wav.getFullPathName().toRawUTF8()) != 0)
            return failed ("it couldn't be replaced");
       #endif

        return {};
    }
}

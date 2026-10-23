// main/vocab_bank.c - the embedded test word bank (spec 12.2) and pure lookups.
//
// Test data only: 48 common primary-school entries (40 words, 8 phrases), 34 with Stage 3
// cloze data. The substituted cloze spellings (e.g. "epple") are never displayed. The table
// is the shape the deferred word-bank pipeline will generate; keep one entry per line group
// and never renumber a uid.
#include "vocab_bank.h"

#include <string.h>

// Entry with Stage 3 cloze data: blank at byte `pos`, correct chunk `right`, lure `wrong`.
#define CLOZE_ENTRY(uid_, cat_, text_, ipa_, zh_, pos_, right_, wrong_, example_, cloze_) \
    { .uid = (uid_), .category = (cat_), .cloze_pos = (pos_), .text = (text_), .ipa = (ipa_), \
      .zh = (zh_), .cloze_right = (right_), .cloze_wrong = (wrong_), .example = (example_), \
      .example_cloze = (cloze_), .example_zh = NULL }

// Entry without cloze data (Stage 3 falls back to Stage 2).
#define PLAIN_ENTRY(uid_, cat_, text_, ipa_, zh_, example_, cloze_) \
    { .uid = (uid_), .category = (cat_), .cloze_pos = 0, .text = (text_), .ipa = (ipa_), \
      .zh = (zh_), .cloze_right = NULL, .cloze_wrong = NULL, .example = (example_), \
      .example_cloze = (cloze_), .example_zh = NULL }

// Categories (internal ids, never displayed): 1 fruit, 2 animals, 3 transport,
// 4 school things, 5 body, 6 colors, 7 food and drink, 8 actions, 9 greetings, 10 classroom.
static const vocab_entry_t BUILTIN_ENTRIES[] = {
    // 1 fruit
    CLOZE_ENTRY(101, 1, "apple", "/ˈæpl/", "苹果", 0, "a", "e",
                "I eat an apple.", "I eat an ____."),
    CLOZE_ENTRY(102, 1, "banana", "/bəˈnɑːnə/", "香蕉", 1, "a", "e",
                "The banana is yellow.", "The ____ is yellow."),
    PLAIN_ENTRY(103, 1, "pear", "/peə/", "梨",
                "This pear is sweet.", "This ____ is sweet."),
    CLOZE_ENTRY(104, 1, "watermelon", "/ˈwɔːtəmelən/", "西瓜", 3, "er", "or",
                "I like watermelon.", "I like ____."),
    // 2 animals
    CLOZE_ENTRY(201, 2, "cat", "/kæt/", "猫", 1, "a", "e",
                "The cat is on the bed.", "The ____ is on the bed."),
    PLAIN_ENTRY(202, 2, "dog", "/dɒɡ/", "狗",
                "My dog can run.", "My ____ can run."),
    CLOZE_ENTRY(203, 2, "duck", "/dʌk/", "鸭子", 1, "u", "a",
                "The duck can swim.", "The ____ can swim."),
    CLOZE_ENTRY(204, 2, "sheep", "/ʃiːp/", "绵羊", 2, "ee", "ea",
                "The sheep is white.", "The ____ is white."),
    CLOZE_ENTRY(205, 2, "tiger", "/ˈtaɪɡə/", "老虎", 3, "er", "or",
                "The tiger is big.", "The ____ is big."),
    CLOZE_ENTRY(206, 2, "elephant", "/ˈelɪfənt/", "大象", 3, "ph", "ff",
                "I see an elephant.", "I see an ____."),
    // 3 transport
    CLOZE_ENTRY(301, 3, "bus", "/bʌs/", "公交车", 1, "u", "a",
                "Let's take the bus.", "Let's take the ____."),
    CLOZE_ENTRY(302, 3, "car", "/kɑː/", "小汽车", 1, "ar", "er",
                "This is my car.", "This is my ____."),
    // Curated (spec 7.1 override): magic-e long i sounds like y ("byke"); i/e would test a
    // short vowel.
    CLOZE_ENTRY(303, 3, "bike", "/baɪk/", "自行车", 1, "i", "y",
                "I ride my bike.", "I ride my ____."),
    CLOZE_ENTRY(304, 3, "boat", "/bəʊt/", "小船", 1, "oa", "ow",
                "I see a boat.", "I see a ____."),
    CLOZE_ENTRY(305, 3, "train", "/treɪn/", "火车", 2, "ai", "ay",
                "The train is fast.", "The ____ is fast."),
    // 4 school things
    PLAIN_ENTRY(401, 4, "pen", "/pen/", "钢笔",
                "I have a pen.", "I have a ____."),
    CLOZE_ENTRY(402, 4, "pencil", "/ˈpensl/", "铅笔", 1, "e", "a",
                "This is my pencil.", "This is my ____."),
    CLOZE_ENTRY(403, 4, "book", "/bʊk/", "书", 1, "oo", "ou",
                "I read a book.", "I read a ____."),
    CLOZE_ENTRY(404, 4, "ruler", "/ˈruːlə/", "尺子", 3, "er", "or",
                "I have a ruler.", "I have a ____."),
    CLOZE_ENTRY(405, 4, "schoolbag", "/ˈskuːlbæɡ/", "书包", 3, "oo", "ou",
                "My schoolbag is new.", "My ____ is new."),
    // 5 body
    CLOZE_ENTRY(501, 5, "ear", "/ɪə/", "耳朵", 0, "ea", "ee",
                "Touch your ear.", "Touch your ____."),
    // Curated (spec 7.1 override): the s says /z/, the real spelling trap; o/a is a weaker
    // lure.
    CLOZE_ENTRY(502, 5, "nose", "/nəʊz/", "鼻子", 2, "s", "z",
                "Touch your nose.", "Touch your ____."),
    CLOZE_ENTRY(503, 5, "mouth", "/maʊθ/", "嘴巴", 1, "ou", "ow",
                "Open your mouth.", "Open your ____."),
    CLOZE_ENTRY(504, 5, "hand", "/hænd/", "手", 1, "a", "e",
                "Raise your hand.", "Raise your ____."),
    CLOZE_ENTRY(505, 5, "foot", "/fʊt/", "脚", 1, "oo", "ou",
                "This is my foot.", "This is my ____."),
    // 6 colors
    PLAIN_ENTRY(601, 6, "red", "/red/", "红色",
                "The apple is red.", "The apple is ____."),
    CLOZE_ENTRY(602, 6, "blue", "/bluː/", "蓝色", 2, "ue", "oo",
                "The sky is blue.", "The sky is ____."),
    CLOZE_ENTRY(603, 6, "green", "/ɡriːn/", "绿色", 2, "ee", "ea",
                "The tree is green.", "The tree is ____."),
    // Curated (spec 7.1 override): e/a ("yallow") is the classic vowel trap; the final ow is
    // easy.
    CLOZE_ENTRY(604, 6, "yellow", "/ˈjeləʊ/", "黄色", 1, "e", "a",
                "The sun is yellow.", "The sun is ____."),
    // 7 food and drink
    PLAIN_ENTRY(701, 7, "bread", "/bred/", "面包",
                "I eat bread.", "I eat ____."),
    CLOZE_ENTRY(702, 7, "milk", "/mɪlk/", "牛奶", 1, "i", "e",
                "I drink milk.", "I drink ____."),
    PLAIN_ENTRY(703, 7, "egg", "/eɡ/", "鸡蛋",
                "I have an egg.", "I have an ____."),
    // Curated (spec 7.1 override): a says /o:/, the real trap ("woter"); the final er is a
    // weak schwa.
    CLOZE_ENTRY(704, 7, "water", "/ˈwɔːtə/", "水", 1, "a", "o",
                "Drink some water.", "Drink some ____."),
    CLOZE_ENTRY(705, 7, "juice", "/dʒuːs/", "果汁", 1, "ui", "oo",
                "I want some juice.", "I want some ____."),
    CLOZE_ENTRY(706, 7, "ice cream", "/ˌaɪs ˈkriːm/", "冰淇淋", 6, "ea", "ee",
                "The ice cream is cold.", "The ____ is cold."),
    // 8 actions
    PLAIN_ENTRY(801, 8, "run", "/rʌn/", "跑",
                "I can run fast.", "I can ____ fast."),
    CLOZE_ENTRY(802, 8, "jump", "/dʒʌmp/", "跳", 1, "u", "a",
                "Let's jump!", "Let's ____!"),
    CLOZE_ENTRY(803, 8, "swim", "/swɪm/", "游泳", 2, "i", "e",
                "I can swim.", "I can ____."),
    CLOZE_ENTRY(804, 8, "sing", "/sɪŋ/", "唱歌", 1, "i", "e",
                "Let's sing a song.", "Let's ____ a song."),
    CLOZE_ENTRY(805, 8, "sleep", "/sliːp/", "睡觉", 2, "ee", "ea",
                "It's time to sleep.", "It's time to ____."),
    // 9 greetings
    // Curated (spec 7.1 override): practise the content word "morning", not the known
    // "Good".
    CLOZE_ENTRY(901, 9, "Good morning", NULL, "早上好", 6, "or", "ar",
                "Good morning, Miss Li!", "____, Miss Li!"),
    PLAIN_ENTRY(902, 9, "Thank you", NULL, "谢谢",
                "Thank you very much.", "____ very much."),
    PLAIN_ENTRY(903, 9, "Goodbye", "/ˌɡʊdˈbaɪ/", "再见",
                "Goodbye, Mum!", "____, Mum!"),
    PLAIN_ENTRY(904, 9, "How are you?", NULL, "你好吗",
                "How are you? I'm fine.", NULL),
    PLAIN_ENTRY(905, 9, "Nice to meet you", NULL, "很高兴见到你",
                "Nice to meet you, too.", NULL),
    // 10 classroom
    PLAIN_ENTRY(1001, 10, "Stand up", NULL, "起立",
                "Stand up, please.", "____, please."),
    PLAIN_ENTRY(1002, 10, "Sit down", NULL, "坐下",
                "Sit down, please.", "____, please."),
    PLAIN_ENTRY(1003, 10, "Open your book", NULL, "打开你的书",
                "Open your book, please.", NULL),
};

static const vocab_bank_t BUILTIN_BANK = {
    .entries = BUILTIN_ENTRIES,
    .count = (vocab_index_t)(sizeof(BUILTIN_ENTRIES) / sizeof(BUILTIN_ENTRIES[0])),
};

const vocab_bank_t *vocab_bank_builtin(void)
{
    return &BUILTIN_BANK;
}

const vocab_entry_t *vocab_bank_at(const vocab_bank_t *bank, vocab_index_t index)
{
    if (!bank || !bank->entries || index >= bank->count) return NULL;
    return &bank->entries[index];
}

bool vocab_bank_find(const vocab_bank_t *bank, vocab_uid_t uid, vocab_index_t *out_index)
{
    if (!bank || !bank->entries) return false;
    for (vocab_index_t i = 0; i < bank->count; i++) {
        if (bank->entries[i].uid == uid) {
            if (out_index) *out_index = i;
            return true;
        }
    }
    return false;
}

vocab_kind_t vocab_entry_kind(const vocab_entry_t *entry)
{
    return (entry && entry->text && strchr(entry->text, ' ')) ? VOCAB_KIND_PHRASE
                                                              : VOCAB_KIND_WORD;
}

bool vocab_entry_has_cloze(const vocab_entry_t *entry)
{
    return entry && entry->cloze_right && entry->cloze_wrong;
}

bool vocab_entry_has_ipa(const vocab_entry_t *entry)
{
    return entry && entry->ipa && entry->ipa[0] != '\0';
}

bool vocab_entry_has_example_cloze(const vocab_entry_t *entry)
{
    return entry && entry->example_cloze && entry->example_cloze[0] != '\0';
}

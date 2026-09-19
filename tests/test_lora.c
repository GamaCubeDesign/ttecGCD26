/*
 * LoRa PHY parameters: the profile table of ADR-0004 and the time-on-air
 * formula.
 *
 * The expected times below were produced by tools/analysis/lora_budget.py,
 * an independent floating-point implementation of the same datasheet
 * formula. The two agreeing to the microsecond is the check; the Python tool
 * is the one the ADRs quote, so a drift here would mean the documentation and
 * the flight software disagree about how long a packet takes.
 */

#include "test_util.h"
#include "gama_lora.h"
#include "gama_tc.h"

int main(void)
{
    TEST_GROUP("lora: the three profiles match ADR-0004");
    {
        const gama_lora_phy_t *safe = gama_lora_profile(GAMA_RATE_SAFE);
        const gama_lora_phy_t *nom  = gama_lora_profile(GAMA_RATE_NOMINAL);
        const gama_lora_phy_t *fast = gama_lora_profile(GAMA_RATE_FAST);
        CHECK(safe != NULL && nom != NULL && fast != NULL);

        CHECK_EQ_INT(safe->sf, 12); CHECK_EQ_INT(safe->cr, 4);
        CHECK_EQ_INT(nom->sf,   9); CHECK_EQ_INT(nom->cr,  1);
        CHECK_EQ_INT(fast->sf,  7); CHECK_EQ_INT(fast->cr, 1);

        const gama_lora_phy_t *all[] = { safe, nom, fast };
        for (size_t i = 0; i < 3; i++) {
            CHECK_EQ_INT(all[i]->bw_hz, 125000);
            CHECK_EQ_INT(all[i]->preamble_len, 8);
            CHECK(all[i]->crc_on);
            CHECK(!all[i]->implicit_header);
        }
    }

    TEST_GROUP("lora: an undefined profile is refused, not defaulted");
    {
        CHECK(gama_lora_profile(GAMA_RATE_COUNT) == NULL);
        CHECK(gama_lora_profile(0xFF) == NULL);
    }

    TEST_GROUP("lora: symbol time and low data rate optimisation");
    {
        CHECK_EQ_INT(gama_lora_symbol_us(gama_lora_profile(GAMA_RATE_SAFE)),    32768);
        CHECK_EQ_INT(gama_lora_symbol_us(gama_lora_profile(GAMA_RATE_NOMINAL)),  4096);
        CHECK_EQ_INT(gama_lora_symbol_us(gama_lora_profile(GAMA_RATE_FAST)),     1024);

        /* Mandatory above 16 ms per symbol: only SAFE, at SF12. */
        CHECK(gama_lora_ldro(gama_lora_profile(GAMA_RATE_SAFE)));
        CHECK(!gama_lora_ldro(gama_lora_profile(GAMA_RATE_NOMINAL)));
        CHECK(!gama_lora_ldro(gama_lora_profile(GAMA_RATE_FAST)));

        /* The threshold itself: SF11 at 125 kHz is 16.384 ms, just over. */
        gama_lora_phy_t sf11 = *gama_lora_profile(GAMA_RATE_NOMINAL);
        sf11.sf = 11;
        CHECK(gama_lora_ldro(&sf11));
        sf11.sf = 10;
        CHECK(!gama_lora_ldro(&sf11));
    }

    TEST_GROUP("lora: time on air matches the independent Python reference");
    {
        static const struct { uint8_t profile; uint8_t len; uint32_t us; } ref[] = {
            /* profile, payload bytes, time on air in microseconds */
            { GAMA_RATE_SAFE,       0,   663552u },
            { GAMA_RATE_SAFE,       7,  1187840u },
            { GAMA_RATE_SAFE,      11,  1449984u },
            { GAMA_RATE_SAFE,      16,  1712128u },
            { GAMA_RATE_SAFE,      32,  2498560u },
            { GAMA_RATE_SAFE,      33,  2498560u },
            { GAMA_RATE_SAFE,     208, 11673600u },
            { GAMA_RATE_SAFE,     247, 13770752u },
            { GAMA_RATE_SAFE,     255, 14032896u },
            { GAMA_RATE_NOMINAL,    0,   103424u },
            { GAMA_RATE_NOMINAL,    7,   123904u },
            { GAMA_RATE_NOMINAL,   11,   144384u },
            { GAMA_RATE_NOMINAL,   16,   164864u },
            { GAMA_RATE_NOMINAL,   32,   246784u },
            { GAMA_RATE_NOMINAL,   33,   246784u },
            { GAMA_RATE_NOMINAL,  208,  1045504u },
            { GAMA_RATE_NOMINAL,  247,  1229824u },
            { GAMA_RATE_NOMINAL,  255,  1250304u },
            { GAMA_RATE_FAST,       0,    25856u },
            { GAMA_RATE_FAST,       7,    36096u },
            { GAMA_RATE_FAST,      11,    41216u },
            { GAMA_RATE_FAST,      16,    51456u },
            { GAMA_RATE_FAST,      32,    71936u },
            { GAMA_RATE_FAST,      33,    71936u },
            { GAMA_RATE_FAST,     208,   327936u },
            { GAMA_RATE_FAST,     247,   389376u },
            { GAMA_RATE_FAST,     255,   399616u },
        };
        for (size_t i = 0; i < sizeof(ref) / sizeof(ref[0]); i++) {
            const gama_lora_phy_t *phy = gama_lora_profile(ref[i].profile);
            CHECK_EQ_INT(gama_lora_toa_us(phy, ref[i].len), ref[i].us);
        }
    }

    TEST_GROUP("lora: the figures quoted in ADR-0004 hold");
    {
        /* 208-byte frame: 1.05 s at NOMINAL, 11.67 s at SAFE, 0.33 s at FAST. */
        CHECK_NEAR(gama_lora_toa_us(gama_lora_profile(GAMA_RATE_NOMINAL), 208) / 1e6, 1.046, 0.001);
        CHECK_NEAR(gama_lora_toa_us(gama_lora_profile(GAMA_RATE_SAFE),    208) / 1e6, 11.674, 0.001);
        CHECK_NEAR(gama_lora_toa_us(gama_lora_profile(GAMA_RATE_FAST),    208) / 1e6, 0.328, 0.001);
    }

    TEST_GROUP("lora: time on air never shrinks as the payload grows");
    {
        for (uint8_t p = 0; p < GAMA_RATE_COUNT; p++) {
            const gama_lora_phy_t *phy = gama_lora_profile(p);
            uint32_t prev = 0;
            for (unsigned len = 0; len <= GAMA_LORA_MAX_PAYLOAD; len++) {
                uint32_t t = gama_lora_toa_us(phy, (uint8_t)len);
                CHECK(t >= prev);
                prev = t;
            }
        }
    }

    TEST_GROUP("lora: the header is decodable well before a frame ends");
    {
        /* Preamble (8 + 4.25) plus 8 header symbols = 20.25 symbols. */
        CHECK_EQ_INT(gama_lora_header_us(gama_lora_profile(GAMA_RATE_NOMINAL)), 82944);
        CHECK_EQ_INT(gama_lora_header_us(gama_lora_profile(GAMA_RATE_SAFE)),   663552);
        for (uint8_t p = 0; p < GAMA_RATE_COUNT; p++) {
            const gama_lora_phy_t *phy = gama_lora_profile(p);
            CHECK(gama_lora_header_us(phy) <= gama_lora_toa_us(phy, 0));
        }
    }

    TEST_SUMMARY("test_lora");
}
